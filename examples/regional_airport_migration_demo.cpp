#include "directory_lp.hpp"
#include "modeling.hpp"
#include "optimistic_migration.hpp"
#include "random.hpp"
#include "simulation.hpp"
#include "transport.hpp"
#include "wire.hpp"
#include "world.hpp"

#if defined(WARPSIM_HAS_MPI)
#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include <mpi.h>
#endif

#include <charconv>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using EventKind = std::uint32_t;

    // Event kinds (model-defined).
    enum : EventKind
    {
        Plane_Land = 60002,  // target=plane
        ServiceDone = 60003, // target=plane

        // Committed output kinds.
        Commit_Depart = 60101,
        Commit_Land = 60102,
    };

    struct LandArgs
    {
        std::uint32_t airport = 0;
        warpsim::EventUid logicalKey = 0;
    };

    struct ServiceDoneArgs
    {
        std::uint32_t airport = 0;
        warpsim::EventUid logicalKey = 0;
    };

    struct CommitDepart
    {
        std::uint32_t plane = 0;
        std::uint32_t fromAirport = 0;
        std::uint32_t toAirport = 0;
        std::uint64_t departTime = 0;
        std::uint64_t landTime = 0;
        warpsim::EventUid logicalKey = 0;
    };

    struct CommitLand
    {
        std::uint32_t plane = 0;
        std::uint32_t airport = 0;
        std::uint64_t landTime = 0;
        warpsim::EventUid logicalKey = 0;
    };

    struct Params
    {
        std::uint32_t regions = 4;
        std::uint32_t airportsPerRegion = 2;
        std::uint32_t planes = 64;
        std::uint64_t endTime = 400;

        std::uint32_t serviceMax = 5;

        // 1D geometry / kinematics (integer, deterministic).
        std::uint64_t regionWidth = 1000;
        std::uint64_t speed = 25; // x-units per time unit

        std::uint64_t seed = 1;

        // If non-zero, only emit committed records for planes where plane%emitPlaneMod==0.
        std::uint32_t emitPlaneMod = 4;

        bool verify = false;
        bool hasExpected = false;
        std::uint64_t expectedHash = 0;
    };

    [[noreturn]] void usage_and_exit()
    {
        std::cerr
            << "Regional airport + migration example (no ticks)\n"
            << "  --regions N              number of region LPs (default 4)\n"
            << "  --airports-per-region N  airports per region (default 2)\n"
            << "  --planes N               number of planes (default 64)\n"
            << "  --end T                  stop time (default 400)\n"
            << "  --service-max N          max service time (default 5)\n"
            << "  --region-width X         width of each region in x-units (default 1000)\n"
            << "  --speed V                plane speed in x-units/time (default 25)\n"
            << "  --seed S                 base seed (default 1)\n"
            << "  --emit-plane-mod M       committed output sampling mod (default 4, 0 disables)\n"
            << "  --verify                 verify against an expected hash\n"
            << "  --expected-hash H        expected global hash (used with --verify)\n";
        std::exit(2);
    }

    bool parse_u32(std::string_view s, std::uint32_t &out)
    {
        unsigned long v = 0;
        auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        if (r.ec != std::errc() || r.ptr != s.data() + s.size() || v > std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }
        out = static_cast<std::uint32_t>(v);
        return true;
    }

    bool parse_u64(std::string_view s, std::uint64_t &out)
    {
        unsigned long long v = 0;
        auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        if (r.ec != std::errc() || r.ptr != s.data() + s.size())
        {
            return false;
        }
        out = static_cast<std::uint64_t>(v);
        return true;
    }

    Params parse_args(int argc, char **argv)
    {
        Params p;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view a(argv[i]);
            auto need = [&]() -> std::string_view
            {
                if (i + 1 >= argc)
                {
                    usage_and_exit();
                }
                return std::string_view(argv[++i]);
            };

            if (a == "--regions")
            {
                if (!parse_u32(need(), p.regions))
                    usage_and_exit();
            }
            else if (a == "--airports-per-region")
            {
                if (!parse_u32(need(), p.airportsPerRegion))
                    usage_and_exit();
            }
            else if (a == "--planes")
            {
                if (!parse_u32(need(), p.planes))
                    usage_and_exit();
            }
            else if (a == "--end")
            {
                if (!parse_u64(need(), p.endTime))
                    usage_and_exit();
            }
            else if (a == "--service-max")
            {
                if (!parse_u32(need(), p.serviceMax))
                    usage_and_exit();
            }
            else if (a == "--region-width")
            {
                if (!parse_u64(need(), p.regionWidth))
                    usage_and_exit();
            }
            else if (a == "--speed")
            {
                if (!parse_u64(need(), p.speed))
                    usage_and_exit();
            }
            else if (a == "--seed")
            {
                if (!parse_u64(need(), p.seed))
                    usage_and_exit();
            }
            else if (a == "--emit-plane-mod")
            {
                if (!parse_u32(need(), p.emitPlaneMod))
                    usage_and_exit();
            }
            else if (a == "--verify")
            {
                p.verify = true;
            }
            else if (a == "--expected-hash")
            {
                p.hasExpected = true;
                if (!parse_u64(need(), p.expectedHash))
                    usage_and_exit();
            }
            else
            {
                usage_and_exit();
            }
        }

        if (p.regions == 0 || p.airportsPerRegion == 0 || p.planes == 0 || p.serviceMax == 0 || p.regionWidth == 0 || p.speed == 0)
        {
            usage_and_exit();
        }
        return p;
    }

    inline std::uint64_t mix64(std::uint64_t a, std::uint64_t b) noexcept
    {
        return warpsim::splitmix64(a ^ warpsim::splitmix64(b));
    }

    inline warpsim::EventUid next_logical_key(std::uint64_t seed,
                                              std::uint32_t plane,
                                              std::uint32_t fromAirport,
                                              std::uint32_t toAirport,
                                              warpsim::EventUid prev) noexcept
    {
        std::uint64_t x = seed;
        x = mix64(x, static_cast<std::uint64_t>(plane));
        x = mix64(x, static_cast<std::uint64_t>(fromAirport));
        x = mix64(x, static_cast<std::uint64_t>(toAirport));
        x = mix64(x, static_cast<std::uint64_t>(prev));
        return static_cast<warpsim::EventUid>(warpsim::splitmix64(x));
    }

    inline std::uint64_t div_ceil(std::uint64_t num, std::uint64_t den)
    {
        return (num + den - 1) / den;
    }

    inline std::uint64_t stable_seq(std::uint32_t kindClass, std::uint32_t plane) noexcept
    {
        // Deterministic, rank-count-independent ordering for same-time events.
        // Layout: [kindClass:4][plane:60]
        // plane+1 ensures sequence is never 0.
        const std::uint64_t k = static_cast<std::uint64_t>(kindClass & 0xFu);
        return (k << 60) | static_cast<std::uint64_t>(plane + 1u);
    }

    inline std::uint32_t region_index_of_lp(warpsim::LPId lp) { return static_cast<std::uint32_t>(lp - 1); }

    inline warpsim::LPId lp_of_region(std::uint32_t regionIdx) { return static_cast<warpsim::LPId>(1 + regionIdx); }

    inline std::uint32_t region_of_airport(const Params &p, std::uint32_t airport)
    {
        return airport / p.airportsPerRegion;
    }

    inline std::uint32_t airport_count(const Params &p)
    {
        return p.regions * p.airportsPerRegion;
    }

    inline std::uint64_t airport_x(const Params &p, std::uint32_t airport)
    {
        const std::uint32_t region = region_of_airport(p, airport);
        const std::uint32_t local = airport % p.airportsPerRegion;
        // Deterministic placement within the region: never exactly on a boundary.
        const std::uint64_t offset = (static_cast<std::uint64_t>(local) + 1) * (p.regionWidth / (static_cast<std::uint64_t>(p.airportsPerRegion) + 1));
        return static_cast<std::uint64_t>(region) * p.regionWidth + offset;
    }

    inline warpsim::EntityId plane_entity(std::uint32_t plane)
    {
        return static_cast<warpsim::EntityId>(0xA11A000000000000ULL | static_cast<std::uint64_t>(plane));
    }

    inline std::uint32_t plane_id_from_entity(warpsim::EntityId e)
    {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(e) & 0xFFFFFFFFu);
    }

    inline std::uint64_t abs_u64_diff(std::uint64_t a, std::uint64_t b)
    {
        return (a >= b) ? (a - b) : (b - a);
    }

    struct Plane
    {
        std::uint32_t plane = 0;

        // Current position / region.
        std::uint32_t region = 0; // 0..regions-1
        std::uint64_t x = 0;

        // Current airport when not in flight.
        std::uint32_t airport = 0;

        // Active flight plan.
        std::uint32_t destAirport = 0;
        std::uint32_t destRegion = 0;
        std::uint64_t destX = 0;
        std::uint64_t speed = 0;
        std::uint8_t inFlight = 0;

        warpsim::EventUid logicalKey = 0;
    };

    inline constexpr warpsim::EntityTypeId PlaneType = 9301;

    struct Token
    {
        warpsim::EntityId planeEntity = 0;
        warpsim::EventUid logicalKey = 0;
    };

    struct AirportState
    {
        bool runwayBusy = false;
        std::deque<Token> q;
        std::uint64_t arrivals = 0;
        std::uint64_t departures = 0;
        std::size_t maxQ = 0;
    };

    class RegionLP final : public warpsim::ILogicalProcess
    {
    public:
        RegionLP(warpsim::LPId id, warpsim::LPId directory, Params p)
            : m_id(id), m_directory(directory), m_params(std::move(p)), m_world(m_types)
        {
            m_types.register_trivial<Plane>(PlaneType);

            const std::uint32_t regionIdx = region_index_of_lp(m_id);
            const std::uint32_t aps = m_params.airportsPerRegion;
            m_airports.resize(aps);

            for (std::uint32_t i = 0; i < aps; ++i)
            {
                const std::uint32_t airport = regionIdx * aps + i;
                (void)airport;
            }
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Seed initial planes owned by this region: each plane starts at an airport in its home region.
            const std::uint32_t totalAirports = airport_count(m_params);
            const std::uint32_t regionIdx = region_index_of_lp(m_id);

            for (std::uint32_t planeId = 0; planeId < m_params.planes; ++planeId)
            {
                const std::uint32_t homeRegion = planeId % m_params.regions;
                if (homeRegion != regionIdx)
                {
                    continue;
                }

                const std::uint32_t homeAirport = (homeRegion * m_params.airportsPerRegion) + (planeId % m_params.airportsPerRegion);
                Plane pl;
                pl.plane = planeId;
                pl.region = homeRegion;
                pl.x = airport_x(m_params, homeAirport);
                pl.airport = homeAirport;
                pl.destAirport = homeAirport;
                pl.destRegion = homeRegion;
                pl.destX = pl.x;
                pl.speed = m_params.speed;
                pl.inFlight = 0;
                pl.logicalKey = next_logical_key(m_params.seed, planeId, /*from=*/homeAirport, /*to=*/homeAirport, /*prev=*/0);

                const warpsim::EntityId eid = plane_entity(planeId);
                m_world.emplace<Plane>(eid, PlaneType, pl);

                // Push the plane into its home airport queue at t=0 and start service if possible.
                // We do this by scheduling a deterministic service completion; this avoids any periodic ticking.
                enqueue_arrival_(sink,
                                 /*ts=*/warpsim::TimeStamp{0, stable_seq(/*kindClass=*/0, planeId)},
                                 /*planeEntity=*/eid,
                                 /*airport=*/homeAirport,
                                 /*logicalKey=*/pl.logicalKey);
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            // Directory migration protocol: install plane state on the new owner.
            if (ev.payload.kind == warpsim::optimistic_migration::DefaultInstallKind)
            {
                const auto [entity, state] = warpsim::optimistic_migration::decode_install(ev);
                if (!has_plane_(entity))
                {
                    m_world.emplace<Plane>(entity, PlaneType, Plane{});
                }
                m_world.deserialize_entity(entity, state);

                // After install, continue flight by scheduling the next boundary crossing if needed.
                // This makes the model robust even if a speculative plane event arrives before install.
                const Plane &pl = m_world.read<Plane>(entity, PlaneType);
                if (pl.inFlight != 0 && pl.region != pl.destRegion)
                {
                    schedule_next_boundary_(ctx, ev.ts, pl);
                }
                return;
            }

            // Plane lands at its destination airport.
            if (ev.payload.kind == Plane_Land)
            {
                const auto a = warpsim::modeling::require<LandArgs>(ev, Plane_Land, "land");
                const warpsim::EntityId e = ev.target;
                if (e == 0)
                {
                    throw std::runtime_error("land must be targeted");
                }

                ctx.request_write(kStateEntity);
                warpsim::WorldTxn txn(m_world, ctx);
                auto &pl = txn.write<Plane>(e, PlaneType);

                // Resolve landing.
                pl.inFlight = 0;
                pl.airport = a.airport;
                pl.region = region_of_airport(m_params, a.airport);
                pl.x = airport_x(m_params, a.airport);
                pl.logicalKey = a.logicalKey;

                // Committed landing record.
                if (m_params.emitPlaneMod != 0 && (pl.plane % m_params.emitPlaneMod) == 0)
                {
                    warpsim::modeling::emit_committed(ctx,
                                                      ev.ts,
                                                      Commit_Land,
                                                      CommitLand{.plane = pl.plane, .airport = a.airport, .landTime = ev.ts.time, .logicalKey = a.logicalKey});
                }

                // Enqueue at the airport and start service if runway is free.
                enqueue_arrival_ctx_(ctx, ev.ts, e, a.airport, a.logicalKey);
                return;
            }

            // Service done: the plane departs and schedules (crossings + landing) as discrete events.
            if (ev.payload.kind == ServiceDone)
            {
                const auto a = warpsim::modeling::require<ServiceDoneArgs>(ev, ServiceDone, "service-done");
                const warpsim::EntityId e = ev.target;
                if (e == 0)
                {
                    throw std::runtime_error("service-done must be targeted");
                }

                ctx.request_write(kStateEntity);
                warpsim::WorldTxn txn(m_world, ctx);
                auto &pl = txn.write<Plane>(e, PlaneType);

                // Mark runway free.
                AirportState &ap = airport_state_(a.airport);
                ap.runwayBusy = false;

                // Choose a destination airport deterministically.
                const std::uint32_t totalAirports = airport_count(m_params);
                std::uint32_t toAirport = a.airport;
                if (totalAirports > 1)
                {
                    const std::uint64_t r = warpsim::rng_u64_range(m_params.seed,
                                                                   /*lp=*/m_id,
                                                                   /*stream=*/10,
                                                                   /*eventUid=*/a.logicalKey,
                                                                   /*lo=*/0,
                                                                   /*hi=*/static_cast<std::uint64_t>(totalAirports - 2),
                                                                   /*draw=*/0);
                    const std::uint32_t candidate = static_cast<std::uint32_t>(r);
                    toAirport = (candidate >= a.airport) ? (candidate + 1) : candidate;
                }

                const warpsim::EventUid nextKey = next_logical_key(m_params.seed, pl.plane, a.airport, toAirport, a.logicalKey);

                // Install flight plan on the plane (at departure time).
                pl.destAirport = toAirport;
                pl.destRegion = region_of_airport(m_params, toAirport);
                pl.destX = airport_x(m_params, toAirport);
                pl.speed = m_params.speed;
                pl.inFlight = 1;
                pl.logicalKey = nextKey;

                // Compute landing time by stepping through region boundaries (integer, deterministic).
                const std::uint64_t landTime = compute_land_time_(ev.ts.time, pl);
                if (landTime <= m_params.endTime)
                {
                    // Emit committed departure record (rollback-safe).
                    if (m_params.emitPlaneMod != 0 && (pl.plane % m_params.emitPlaneMod) == 0)
                    {
                        warpsim::modeling::emit_committed(ctx,
                                                          ev.ts,
                                                          Commit_Depart,
                                                          CommitDepart{.plane = pl.plane,
                                                                       .fromAirport = a.airport,
                                                                       .toAirport = toAirport,
                                                                       .departTime = ev.ts.time,
                                                                       .landTime = landTime,
                                                                       .logicalKey = a.logicalKey});
                    }

                    // Schedule landing now; directory routing will deliver it to the owner-at-time.
                    warpsim::modeling::send_targeted(ctx,
                                                     warpsim::TimeStamp{landTime, stable_seq(/*kindClass=*/3, pl.plane)},
                                                     /*src=*/m_id,
                                                     /*target=*/e,
                                                     /*kind=*/Plane_Land,
                                                     LandArgs{.airport = toAirport, .logicalKey = nextKey});

                    // If needed, schedule the first boundary crossing (migration + enter-region).
                    if (pl.region != pl.destRegion)
                    {
                        schedule_next_boundary_(ctx, ev.ts, pl);
                    }
                }

                ap.departures += 1;

                // Start service for next queued plane at this airport.
                if (!ap.q.empty())
                {
                    start_next_(ctx, ev.ts, a.airport);
                }
                return;
            }

            throw std::runtime_error("regional_airport_migration_demo: unexpected event kind");
        }

        warpsim::ByteBuffer save_state() const override
        {
            warpsim::WireWriter w;
            w.write_u32(static_cast<std::uint32_t>(m_airports.size()));
            for (const auto &ap : m_airports)
            {
                w.write_u8(static_cast<std::uint8_t>(ap.runwayBusy ? 1 : 0));
                w.write_u64(ap.arrivals);
                w.write_u64(ap.departures);
                w.write_u64(static_cast<std::uint64_t>(ap.maxQ));

                w.write_u32(static_cast<std::uint32_t>(ap.q.size()));
                for (const auto &t : ap.q)
                {
                    w.write_u64(static_cast<std::uint64_t>(t.planeEntity));
                    w.write_u64(static_cast<std::uint64_t>(t.logicalKey));
                }
            }
            return w.take();
        }

        void load_state(std::span<const std::byte> bytes) override
        {
            warpsim::WireReader r(bytes);
            const std::uint32_t n = r.read_u32();
            if (n != m_airports.size())
            {
                throw std::runtime_error("regional_airport_migration_demo: airport state mismatch");
            }

            for (auto &ap : m_airports)
            {
                ap.runwayBusy = (r.read_u8() != 0);
                ap.arrivals = r.read_u64();
                ap.departures = r.read_u64();
                ap.maxQ = static_cast<std::size_t>(r.read_u64());

                ap.q.clear();
                const std::uint32_t qn = r.read_u32();
                ap.q.resize(qn);
                for (std::uint32_t i = 0; i < qn; ++i)
                {
                    ap.q[i].planeEntity = static_cast<warpsim::EntityId>(r.read_u64());
                    ap.q[i].logicalKey = static_cast<warpsim::EventUid>(r.read_u64());
                }
            }
        }

        std::uint64_t local_arrivals() const
        {
            std::uint64_t total = 0;
            for (const auto &ap : m_airports)
            {
                total += ap.arrivals;
            }
            return total;
        }

        std::uint64_t local_departures() const
        {
            std::uint64_t total = 0;
            for (const auto &ap : m_airports)
            {
                total += ap.departures;
            }
            return total;
        }

    private:
        // We snapshot LP state on any airport queue/runway mutation.
        static constexpr warpsim::EntityId kStateEntity = 0xA1F0F0A1ULL;

        bool has_plane_(warpsim::EntityId e) const
        {
            try
            {
                (void)m_world.type_of(e);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        AirportState &airport_state_(std::uint32_t airport)
        {
            const std::uint32_t regionIdx = region_index_of_lp(m_id);
            const std::uint32_t local = airport - (regionIdx * m_params.airportsPerRegion);
            if (local >= m_airports.size())
            {
                throw std::runtime_error("regional_airport_migration_demo: airport not in this region");
            }
            return m_airports[local];
        }

        const AirportState &airport_state_(std::uint32_t airport) const
        {
            return const_cast<RegionLP *>(this)->airport_state_(airport);
        }

        void enqueue_arrival_(warpsim::IEventSink &sink,
                              warpsim::TimeStamp ts,
                              warpsim::EntityId planeE,
                              std::uint32_t airport,
                              warpsim::EventUid logicalKey)
        {
            // Seed-time arrival injection via scheduling a service completion if the runway is free.
            // This avoids introducing any periodic ticks.
            AirportState &ap = airport_state_(airport);
            ap.arrivals += 1;
            ap.q.push_back(Token{.planeEntity = planeE, .logicalKey = logicalKey});
            if (ap.q.size() > ap.maxQ)
            {
                ap.maxQ = ap.q.size();
            }

            if (!ap.runwayBusy)
            {
                // Start service at ts.
                start_next_sink_(sink, ts, airport);
            }
        }

        void enqueue_arrival_ctx_(warpsim::IEventContext &ctx,
                                  warpsim::TimeStamp ts,
                                  warpsim::EntityId planeE,
                                  std::uint32_t airport,
                                  warpsim::EventUid logicalKey)
        {
            AirportState &ap = airport_state_(airport);
            ap.arrivals += 1;
            ap.q.push_back(Token{.planeEntity = planeE, .logicalKey = logicalKey});
            if (ap.q.size() > ap.maxQ)
            {
                ap.maxQ = ap.q.size();
            }
            if (!ap.runwayBusy)
            {
                start_next_(ctx, ts, airport);
            }
        }

        void start_next_sink_(warpsim::IEventSink &sink, warpsim::TimeStamp now, std::uint32_t airport)
        {
            AirportState &ap = airport_state_(airport);
            if (ap.runwayBusy || ap.q.empty())
            {
                return;
            }

            const Token t = ap.q.front();
            ap.q.pop_front();
            ap.runwayBusy = true;

            const std::uint32_t planeId = plane_id_from_entity(t.planeEntity);
            const std::uint64_t serviceDelta = 1 + warpsim::rng_u64_range(m_params.seed,
                                                                          /*lp=*/m_id,
                                                                          /*stream=*/11,
                                                                          /*eventUid=*/t.logicalKey,
                                                                          /*lo=*/1,
                                                                          /*hi=*/m_params.serviceMax,
                                                                          /*draw=*/0);
            const std::uint64_t doneTime = now.time + serviceDelta;
            if (doneTime > m_params.endTime)
            {
                ap.runwayBusy = false;
                return;
            }

            warpsim::modeling::send_targeted(sink,
                                             warpsim::TimeStamp{doneTime, stable_seq(/*kindClass=*/4, planeId)},
                                             /*src=*/m_id,
                                             /*target=*/t.planeEntity,
                                             /*kind=*/ServiceDone,
                                             ServiceDoneArgs{.airport = airport, .logicalKey = t.logicalKey});
        }

        void start_next_(warpsim::IEventContext &ctx, warpsim::TimeStamp now, std::uint32_t airport)
        {
            AirportState &ap = airport_state_(airport);
            if (ap.runwayBusy || ap.q.empty())
            {
                return;
            }

            const Token t = ap.q.front();
            ap.q.pop_front();
            ap.runwayBusy = true;

            const std::uint32_t planeId = plane_id_from_entity(t.planeEntity);
            const std::uint64_t serviceDelta = 1 + warpsim::rng_u64_range(m_params.seed,
                                                                          /*lp=*/m_id,
                                                                          /*stream=*/11,
                                                                          /*eventUid=*/t.logicalKey,
                                                                          /*lo=*/1,
                                                                          /*hi=*/m_params.serviceMax,
                                                                          /*draw=*/0);
            const std::uint64_t doneTime = now.time + serviceDelta;
            if (doneTime > m_params.endTime)
            {
                ap.runwayBusy = false;
                return;
            }

            warpsim::modeling::send_targeted(ctx,
                                             warpsim::TimeStamp{doneTime, stable_seq(/*kindClass=*/4, planeId)},
                                             /*src=*/m_id,
                                             /*target=*/t.planeEntity,
                                             /*kind=*/ServiceDone,
                                             ServiceDoneArgs{.airport = airport, .logicalKey = t.logicalKey});
        }

        std::uint64_t compute_land_time_(std::uint64_t departTime, const Plane &pl) const
        {
            std::uint64_t t = departTime;
            std::uint32_t region = pl.region;
            std::uint64_t x = pl.x;

            while (region != pl.destRegion)
            {
                const bool goingRight = (pl.destRegion > region);
                const std::uint64_t boundaryX = goingRight ? (static_cast<std::uint64_t>(region + 1) * m_params.regionWidth)
                                                           : (static_cast<std::uint64_t>(region) * m_params.regionWidth);
                const std::uint64_t dist = abs_u64_diff(boundaryX, x);
                const std::uint64_t dt = div_ceil(dist, pl.speed);
                t += (dt == 0) ? 1 : dt;
                x = boundaryX;
                region = goingRight ? (region + 1) : (region - 1);
            }

            const std::uint64_t finalDist = abs_u64_diff(pl.destX, x);
            const std::uint64_t finalDt = div_ceil(finalDist, pl.speed);
            t += (finalDt == 0) ? 1 : finalDt;
            return t;
        }

        void schedule_next_boundary_(warpsim::IEventContext &ctx, warpsim::TimeStamp now, const Plane &plAtNow)
        {
            if (plAtNow.region == plAtNow.destRegion)
            {
                return;
            }

            const bool goingRight = (plAtNow.destRegion > plAtNow.region);
            const std::uint32_t nextRegion = goingRight ? (plAtNow.region + 1) : (plAtNow.region - 1);
            const warpsim::LPId nextOwner = lp_of_region(nextRegion);

            const std::uint64_t boundaryX = goingRight ? (static_cast<std::uint64_t>(plAtNow.region + 1) * m_params.regionWidth)
                                                       : (static_cast<std::uint64_t>(plAtNow.region) * m_params.regionWidth);
            const std::uint64_t dist = abs_u64_diff(boundaryX, plAtNow.x);
            const std::uint64_t dt = div_ceil(dist, plAtNow.speed);
            const std::uint64_t crossTime = now.time + ((dt == 0) ? 1 : dt);

            if (crossTime > m_params.endTime)
            {
                return;
            }

            const std::uint32_t planeId = plAtNow.plane;

            // Prepare the plane state at the moment it crosses into nextRegion.
            Plane atCross = plAtNow;
            atCross.region = nextRegion;
            atCross.x = boundaryX;

            const auto bytes = warpsim::bytes_from_trivially_copyable(atCross);

            // Deterministic ordering at the crossing timestamp:
            // - update-owner/install: seq(kindClass=1)
            // There is no separate "enter" event; the install handler will schedule the next boundary.
            const warpsim::TimeStamp updTs{crossTime, stable_seq(/*kindClass=*/1, planeId)};

            warpsim::optimistic_migration::send_update_owner(ctx,
                                                             updTs,
                                                             /*src=*/m_id,
                                                             /*directoryLp=*/m_directory,
                                                             /*updateOwnerKind=*/warpsim::optimistic_migration::DefaultUpdateOwnerKind,
                                                             /*entity=*/plane_entity(planeId),
                                                             /*newOwner=*/nextOwner,
                                                             std::span<const std::byte>(bytes.data(), bytes.size()));
        }

        warpsim::LPId m_id = 0;
        warpsim::LPId m_directory = 0;
        Params m_params;

        warpsim::EntityTypeRegistry m_types;
        warpsim::World m_world;

        std::vector<AirportState> m_airports;
    };
}

int main(int argc, char **argv)
{
    const Params p = parse_args(argc, argv);

#if defined(WARPSIM_HAS_MPI)
    int provided = 0;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_SINGLE, &provided);

    int rank = 0;
    int size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
#else
    const int rank = 0;
    const int size = 1;
#endif

    // Directory LP id is stable; keep it off the region id range.
    constexpr warpsim::LPId kDirectoryLp = 10000;

    std::shared_ptr<warpsim::ITransport> transport;
#if defined(WARPSIM_HAS_MPI)
    if (size > 1)
    {
        transport = std::make_shared<warpsim::MpiTransport>(MPI_COMM_WORLD, /*tag=*/9);
    }
    else
#endif
    {
        transport = std::make_shared<warpsim::InProcTransport>();
    }

    warpsim::SimulationConfig cfg;
    cfg.rank = static_cast<warpsim::RankId>(rank);
    cfg.seed = p.seed;

#if defined(WARPSIM_HAS_MPI)
    if (size > 1)
    {
        cfg.lpToRank = [size](warpsim::LPId lp) -> warpsim::RankId
        {
            // Spread region LPs; directory may land on any rank.
            return static_cast<warpsim::RankId>(static_cast<int>(lp % static_cast<warpsim::LPId>(size)));
        };
        cfg.gvtReduceMin = [=](warpsim::TimeStamp local)
        { return warpsim::mpi_allreduce_min_timestamp(MPI_COMM_WORLD, local); };
        cfg.anyRankHasWork = [=](bool local)
        { return warpsim::mpi_allreduce_any_work(MPI_COMM_WORLD, local); };
    }
#endif

    // Route all plane-targeted events through the directory.
    cfg.directoryForEntity = [](warpsim::EntityId) -> warpsim::LPId
    { return kDirectoryLp; };

    // Engine guardrails for optimistic migration:
    // - Buffer targeted events that arrive before install.
    // - Ensure migration control events order before normal events at the same timestamp.
    cfg.enableInstallBeforeUseBuffering = true;
    cfg.installEventKind = warpsim::optimistic_migration::DefaultInstallKind;
    cfg.isControlEvent = [](const warpsim::Event &ev) -> bool
    {
        return ev.payload.kind == warpsim::optimistic_migration::DefaultUpdateOwnerKind ||
               ev.payload.kind == warpsim::optimistic_migration::DefaultInstallKind ||
               ev.payload.kind == warpsim::optimistic_migration::DefaultFossilCollectKind;
    };

    // Committed sink: hash committed records + directory migration logs.
    std::uint64_t committedHash = 0;
    cfg.committedSink = [&](warpsim::RankId, warpsim::LPId, warpsim::TimeStamp ts, const warpsim::Payload &payload)
    {
        if (payload.kind == Commit_Depart)
        {
            if (payload.bytes.size() != sizeof(CommitDepart))
            {
                throw std::runtime_error("regional_airport_migration_demo: bad Commit_Depart payload");
            }
            CommitDepart rec{};
            std::memcpy(&rec, payload.bytes.data(), sizeof(rec));

            std::uint64_t x = 0;
            x = mix64(x, rec.plane);
            x = mix64(x, rec.fromAirport);
            x = mix64(x, rec.toAirport);
            x = mix64(x, rec.departTime);
            x = mix64(x, rec.landTime);
            x = mix64(x, static_cast<std::uint64_t>(rec.logicalKey));
            committedHash ^= warpsim::splitmix64(x);
            return;
        }

        if (payload.kind == Commit_Land)
        {
            if (payload.bytes.size() != sizeof(CommitLand))
            {
                throw std::runtime_error("regional_airport_migration_demo: bad Commit_Land payload");
            }
            CommitLand rec{};
            std::memcpy(&rec, payload.bytes.data(), sizeof(rec));

            std::uint64_t x = 0;
            x = mix64(x, rec.plane);
            x = mix64(x, rec.airport);
            x = mix64(x, rec.landTime);
            x = mix64(x, static_cast<std::uint64_t>(rec.logicalKey));
            committedHash ^= warpsim::splitmix64(x);
            return;
        }

        if (payload.kind == warpsim::optimistic_migration::DefaultMigrationLogKind)
        {
            const auto r = warpsim::optimistic_migration::decode_migration_log_payload(payload);
            std::uint64_t x = 0;
            x = mix64(x, static_cast<std::uint64_t>(r.entity));
            x = mix64(x, static_cast<std::uint64_t>(r.oldOwner));
            x = mix64(x, static_cast<std::uint64_t>(r.newOwner));
            x = mix64(x, static_cast<std::uint64_t>(ts.time));
            committedHash ^= warpsim::splitmix64(x);
            return;
        }
    };

    warpsim::Simulation sim(cfg, transport);

    const auto owns_lp = [&](warpsim::LPId id) -> bool
    {
        if (!cfg.lpToRank)
        {
            return true;
        }
        return cfg.lpToRank(id) == cfg.rank;
    };

    // Add directory LP on its owning rank.
    if (owns_lp(kDirectoryLp))
    {
        auto dirCfg = warpsim::optimistic_migration::make_default_directory_config(kDirectoryLp, /*stateEntityId=*/0xD1EC7003u);

        // Default owner before any migrations: planeId % regions.
        dirCfg.defaultOwner = [p](warpsim::EntityId entity, warpsim::TimeStamp) -> warpsim::LPId
        {
            const std::uint32_t planeId = static_cast<std::uint32_t>(static_cast<std::uint64_t>(entity) & 0xFFFFFFFFu);
            const std::uint32_t region = (p.regions == 0) ? 0 : (planeId % p.regions);
            return static_cast<warpsim::LPId>(1 + region);
        };

        sim.add_lp(std::make_unique<warpsim::DirectoryLP>(std::move(dirCfg)));
    }

    std::vector<RegionLP *> localRegions;
    for (std::uint32_t r = 0; r < p.regions; ++r)
    {
        const warpsim::LPId id = static_cast<warpsim::LPId>(1 + r);
        if (!owns_lp(id))
        {
            continue;
        }
        auto lp = std::make_unique<RegionLP>(id, kDirectoryLp, p);
        localRegions.push_back(lp.get());
        sim.add_lp(std::move(lp));
    }

    sim.run();

    std::uint64_t globalHash = committedHash;
    std::uint64_t localArrivals = 0;
    std::uint64_t localDepartures = 0;

    for (const auto *lp : localRegions)
    {
        localArrivals += lp->local_arrivals();
        localDepartures += lp->local_departures();
    }

#if defined(WARPSIM_HAS_MPI)
    if (size > 1)
    {
        std::uint64_t hashOut = 0;
        MPI_Allreduce(&globalHash, &hashOut, 1, MPI_UINT64_T, MPI_BXOR, MPI_COMM_WORLD);
        globalHash = hashOut;

        std::uint64_t arrOut = 0;
        std::uint64_t depOut = 0;
        MPI_Allreduce(&localArrivals, &arrOut, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
        MPI_Allreduce(&localDepartures, &depOut, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
        localArrivals = arrOut;
        localDepartures = depOut;
    }
#endif

    if (rank == 0)
    {
        std::cout << "regional_airport_migration_demo hash=" << globalHash
                  << " arrivals=" << localArrivals
                  << " departures=" << localDepartures
                  << "\n";
    }

    if (p.verify)
    {
        if (!p.hasExpected)
        {
            if (rank == 0)
            {
                std::cerr << "--verify requires --expected-hash\n";
            }
#if defined(WARPSIM_HAS_MPI)
            MPI_Finalize();
#endif
            return 2;
        }

        if (globalHash != p.expectedHash)
        {
            if (rank == 0)
            {
                std::cerr << "regional_airport_migration_demo verify failed: expected=" << p.expectedHash << " got=" << globalHash << "\n";
            }
#if defined(WARPSIM_HAS_MPI)
            MPI_Finalize();
#endif
            return 3;
        }

        if (rank == 0)
        {
            std::cout << "regional_airport_migration_demo ok\n";
        }
    }

#if defined(WARPSIM_HAS_MPI)
    MPI_Finalize();
#endif
    return 0;
}
