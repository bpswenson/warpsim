#include "random.hpp"
#include "simulation.hpp"
#include "transport.hpp"

#if defined(WARPSIM_HAS_MPI)
#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include <mpi.h>
#endif

#include <charconv>
#include <cstdint>
#include <cstring>
#include <deque>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
    using EventKind = std::uint32_t;

    inline std::uint64_t stable_seq(std::uint32_t kindClass, warpsim::LPId dst, std::uint32_t plane)
    {
        // Deterministic, rank-count-independent ordering for same-time events.
        // Layout: [kindClass:2][dst:32][plane:30]
        // - kindClass: 0=arrival, 1=service-done (others reserved)
        // - plane+1 ensures sequence is never 0
        if (plane >= (1u << 30))
        {
            throw std::runtime_error("airport: plane id too large for stable_seq");
        }
        const std::uint64_t k = static_cast<std::uint64_t>(kindClass & 0x3u);
        return (k << 62) | (static_cast<std::uint64_t>(dst) << 30) | static_cast<std::uint64_t>(plane + 1u);
    }

    enum : EventKind
    {
        PlaneArrive = 9001,
        ServiceDone = 9002,

        // Committed output kinds.
        Commit_Departure = 9101,
    };

    struct PlaneArriveArgs
    {
        std::uint32_t plane = 0;
        warpsim::EventUid logicalKey = 0;
    };

    struct ServiceDoneArgs
    {
        std::uint32_t plane = 0;
        warpsim::EventUid logicalKey = 0;
    };

    struct CommitDeparture
    {
        std::uint32_t plane = 0;
        std::uint32_t fromAirport = 0;
        std::uint32_t toAirport = 0;
        std::uint64_t departTime = 0;
        std::uint64_t arriveTime = 0;
        warpsim::EventUid logicalKey = 0;
    };

    struct Params
    {
        std::uint32_t airports = 64;
        std::uint32_t planes = 4096;
        std::uint64_t endTime = 20000;

        std::uint32_t serviceMax = 10;
        std::uint32_t flightMax = 40;

        std::uint64_t seed = 1;

        // If non-zero, emit committed departure records only for planes where (plane % emitPlaneMod == 0).
        std::uint32_t emitPlaneMod = 32;

        // Verification mode for tests.
        bool verify = false;
        std::uint64_t expectedHash = 0;
        bool hasExpected = false;
    };

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

    [[noreturn]] void usage_and_exit()
    {
        std::cerr << "Fujimoto-style airport queueing example\n"
                  << "  --airports N          number of airports (default 64)\n"
                  << "  --planes N            number of planes (default 4096)\n"
                  << "  --end T               stop time (default 20000)\n"
                  << "  --service-max N       max service time (default 10)\n"
                  << "  --flight-max N        max flight time (default 40)\n"
                  << "  --seed S              base seed (default 1)\n"
                  << "  --emit-plane-mod M    emit committed output only for planes where plane%M==0 (default 32, 0 disables)\n"
                  << "  --verify              verify against an expected hash\n"
                  << "  --expected-hash H     expected global hash (used with --verify)\n";
        std::exit(2);
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

            if (a == "--airports")
            {
                if (!parse_u32(need(), p.airports))
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
            else if (a == "--flight-max")
            {
                if (!parse_u32(need(), p.flightMax))
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

        if (p.airports == 0 || p.planes == 0 || p.serviceMax == 0 || p.flightMax == 0)
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
                                              warpsim::LPId from,
                                              warpsim::LPId to,
                                              warpsim::EventUid prev) noexcept
    {
        std::uint64_t x = seed;
        x = mix64(x, static_cast<std::uint64_t>(plane));
        x = mix64(x, static_cast<std::uint64_t>(from));
        x = mix64(x, static_cast<std::uint64_t>(to));
        x = mix64(x, static_cast<std::uint64_t>(prev));
        return static_cast<warpsim::EventUid>(warpsim::splitmix64(x));
    }

    struct Token
    {
        std::uint32_t plane = 0;
        warpsim::EventUid logicalKey = 0;
    };

    class AirportLP final : public warpsim::ILogicalProcess
    {
    public:
        AirportLP(warpsim::LPId id, Params params)
            : m_id(id), m_params(params)
        {
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            // All airport behavior mutates queue/runway state.
            ctx.request_write(kStateEntity);

            if (ev.payload.kind == PlaneArrive)
            {
                const auto a = decode_<PlaneArriveArgs>(ev);
                ++m_arrivals;
                m_q.push_back(Token{a.plane, a.logicalKey});
                if (m_q.size() > m_maxQ)
                {
                    m_maxQ = m_q.size();
                }
                if (!m_runwayBusy)
                {
                    start_next_(ev.ts, ctx);
                }
                return;
            }

            if (ev.payload.kind == ServiceDone)
            {
                const auto a = decode_<ServiceDoneArgs>(ev);
                (void)a;

                m_runwayBusy = false;

                // Choose destination + flight time deterministically using the logical key.
                const warpsim::EventUid key = a.logicalKey;

                warpsim::LPId to = m_id;
                if (m_params.airports > 1)
                {
                    const std::uint64_t r = warpsim::rng_u64_range(m_params.seed, m_id, /*stream=*/2, key,
                                                                   /*lo=*/0, /*hi=*/m_params.airports - 2, /*draw=*/0);
                    // Map r in [0..airports-2] to an airport in [1..airports] excluding m_id.
                    const warpsim::LPId candidate = static_cast<warpsim::LPId>(1 + r);
                    to = (candidate >= m_id) ? static_cast<warpsim::LPId>(candidate + 1) : candidate;
                }

                const std::uint64_t flightDelta = 1 + warpsim::rng_u64_range(m_params.seed, m_id, /*stream=*/3, key,
                                                                             /*lo=*/1, /*hi=*/m_params.flightMax, /*draw=*/0);
                const std::uint64_t arriveTime = ev.ts.time + flightDelta;
                const warpsim::EventUid nextKey = next_logical_key(m_params.seed, a.plane, m_id, to, key);

                // Emit committed output for a subset of planes.
                if (m_params.emitPlaneMod != 0 && (a.plane % m_params.emitPlaneMod) == 0)
                {
                    CommitDeparture rec;
                    rec.plane = a.plane;
                    rec.fromAirport = static_cast<std::uint32_t>(m_id);
                    rec.toAirport = static_cast<std::uint32_t>(to);
                    rec.departTime = ev.ts.time;
                    rec.arriveTime = arriveTime;
                    rec.logicalKey = key;

                    warpsim::Payload p;
                    p.kind = Commit_Departure;
                    p.bytes = warpsim::bytes_from_trivially_copyable(rec);
                    ctx.emit_committed(ev.ts, std::move(p));
                }

                // Send arrival to the destination airport.
                if (arriveTime <= m_params.endTime)
                {
                    warpsim::Event next;
                    next.ts = warpsim::TimeStamp{arriveTime, stable_seq(/*kindClass=*/0, to, a.plane)};
                    next.src = m_id;
                    next.dst = to;
                    next.payload.kind = PlaneArrive;
                    next.payload.bytes = warpsim::bytes_from_trivially_copyable(PlaneArriveArgs{.plane = a.plane, .logicalKey = nextKey});
                    ctx.send(std::move(next));
                }

                ++m_departures;

                // Start service for next queued plane if any.
                if (!m_q.empty())
                {
                    start_next_(ev.ts, ctx);
                }
                return;
            }
        }

        warpsim::ByteBuffer save_state() const override
        {
            warpsim::WireWriter w;
            w.write_u8(static_cast<std::uint8_t>(m_runwayBusy ? 1 : 0));
            w.write_u64(m_arrivals);
            w.write_u64(m_departures);
            w.write_u64(static_cast<std::uint64_t>(m_maxQ));

            w.write_u32(static_cast<std::uint32_t>(m_q.size()));
            for (const auto &t : m_q)
            {
                w.write_u32(t.plane);
                w.write_u64(static_cast<std::uint64_t>(t.logicalKey));
            }
            return w.take();
        }

        void load_state(std::span<const std::byte> bytes) override
        {
            warpsim::WireReader r(bytes);
            m_runwayBusy = (r.read_u8() != 0);
            m_arrivals = r.read_u64();
            m_departures = r.read_u64();
            m_maxQ = static_cast<std::size_t>(r.read_u64());

            m_q.clear();
            const std::uint32_t n = r.read_u32();
            m_q.resize(n);
            for (std::uint32_t i = 0; i < n; ++i)
            {
                m_q[i].plane = r.read_u32();
                m_q[i].logicalKey = static_cast<warpsim::EventUid>(r.read_u64());
            }
        }

        std::uint64_t arrivals() const { return m_arrivals; }
        std::uint64_t departures() const { return m_departures; }
        std::size_t max_queue() const { return m_maxQ; }

    private:
        template <class T>
        static T decode_(const warpsim::Event &ev)
        {
            if (ev.payload.bytes.size() != sizeof(T))
            {
                throw std::runtime_error("airport: bad payload");
            }
            T out{};
            std::memcpy(&out, ev.payload.bytes.data(), sizeof(T));
            return out;
        }

        void start_next_(warpsim::TimeStamp now, warpsim::IEventContext &ctx)
        {
            if (m_runwayBusy)
            {
                return;
            }
            if (m_q.empty())
            {
                return;
            }

            const Token t = m_q.front();
            m_q.pop_front();

            m_runwayBusy = true;

            const std::uint64_t serviceDelta = 1 + warpsim::rng_u64_range(m_params.seed, m_id, /*stream=*/1, t.logicalKey,
                                                                          /*lo=*/1, /*hi=*/m_params.serviceMax, /*draw=*/0);
            const std::uint64_t doneTime = now.time + serviceDelta;

            if (doneTime > m_params.endTime)
            {
                // If we're past the horizon, just stop servicing.
                m_runwayBusy = false;
                return;
            }

            warpsim::Event e;
            e.ts = warpsim::TimeStamp{doneTime, stable_seq(/*kindClass=*/1, m_id, t.plane)};
            e.src = m_id;
            e.dst = m_id;
            e.payload.kind = ServiceDone;
            e.payload.bytes = warpsim::bytes_from_trivially_copyable(ServiceDoneArgs{.plane = t.plane, .logicalKey = t.logicalKey});
            ctx.send(std::move(e));
        }

        static constexpr warpsim::EntityId kStateEntity = 0xA1F0F007ULL;

        warpsim::LPId m_id = 0;
        Params m_params;

        bool m_runwayBusy = false;
        std::deque<Token> m_q;

        std::uint64_t m_arrivals = 0;
        std::uint64_t m_departures = 0;
        std::size_t m_maxQ = 0;
    };

    // Driver seeds initial planes.
    class DriverLP final : public warpsim::ILogicalProcess
    {
    public:
        DriverLP(warpsim::LPId id, Params params) : m_id(id), m_params(params) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            for (std::uint32_t plane = 0; plane < m_params.planes; ++plane)
            {
                const warpsim::LPId airport = static_cast<warpsim::LPId>(1 + (plane % m_params.airports));

                const warpsim::EventUid key = next_logical_key(m_params.seed, plane, /*from=*/0, /*to=*/airport, /*prev=*/0);

                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{0, stable_seq(/*kindClass=*/0, airport, plane)};
                ev.src = m_id;
                ev.dst = airport;
                ev.payload.kind = PlaneArrive;
                ev.payload.bytes = warpsim::bytes_from_trivially_copyable(PlaneArriveArgs{.plane = plane, .logicalKey = key});
                sink.send(std::move(ev));
            }
        }

        void on_event(const warpsim::Event &, warpsim::IEventContext &) override {}

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        Params m_params;
    };
}

int main(int argc, char **argv)
{
    const Params p = parse_args(argc, argv);

    const bool debug = (std::getenv("WARPSIM_AIRPORT_DEBUG") != nullptr);

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

    const warpsim::LPId driverId = static_cast<warpsim::LPId>(1 + p.airports);

#if defined(WARPSIM_HAS_MPI)
    // NOTE: This example is intended to be deterministic across MPI rank counts.
    // Today we run the full model on rank 0 and broadcast a deterministic summary
    // (including committed-output hash) for verification. This keeps the example
    // usable under mpiexec while avoiding multi-rank optimistic rollback thrash for
    // this specific workload.
    if (size > 1 && rank != 0)
    {
        std::uint64_t globalHash = 0;
        std::uint64_t totalArrivals = 0;
        std::uint64_t totalDepartures = 0;
        std::uint64_t maxQ = 0;

        MPI_Bcast(&globalHash, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
        MPI_Bcast(&totalArrivals, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
        MPI_Bcast(&totalDepartures, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
        MPI_Bcast(&maxQ, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);

        if (p.verify)
        {
            if (!p.hasExpected)
            {
                MPI_Finalize();
                return 2;
            }
            if (globalHash != p.expectedHash)
            {
                MPI_Finalize();
                return 3;
            }
        }

        MPI_Finalize();
        return 0;
    }
#endif

    // Use InProcTransport when size==1 to avoid MPI self-send behavior.
    std::shared_ptr<warpsim::ITransport> transport;
#if defined(WARPSIM_HAS_MPI)
    if (size > 1)
    {
        // Rank 0 runs the full simulation locally; no cross-rank traffic.
        transport = std::make_shared<warpsim::InProcTransport>();
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
        // Single-rank execution on rank 0; no routing/collectives needed.
    }
#endif

    // Committed output sink: hash departure records (order-independent combine).
    std::uint64_t committedHash = 0;
    cfg.committedSink = [&](warpsim::RankId, warpsim::LPId, warpsim::TimeStamp, const warpsim::Payload &payload)
    {
        if (payload.kind != Commit_Departure)
        {
            return;
        }
        if (payload.bytes.size() != sizeof(CommitDeparture))
        {
            throw std::runtime_error("airport: bad committed payload");
        }
        CommitDeparture rec{};
        std::memcpy(&rec, payload.bytes.data(), sizeof(rec));

        std::uint64_t x = 0;
        x = mix64(x, rec.plane);
        x = mix64(x, rec.fromAirport);
        x = mix64(x, rec.toAirport);
        x = mix64(x, rec.departTime);
        x = mix64(x, rec.arriveTime);
        x = mix64(x, static_cast<std::uint64_t>(rec.logicalKey));

        committedHash ^= warpsim::splitmix64(x);
    };

    warpsim::Simulation sim(cfg, transport);

    const auto owns_lp = [&](warpsim::LPId) -> bool
    { return true; };

    // Create airport LPs with ids [1..airports].
    std::vector<AirportLP *> airports;
    airports.reserve(p.airports);

    std::uint32_t localAirportCount = 0;
    bool hasDriver = false;

    for (std::uint32_t i = 0; i < p.airports; ++i)
    {
        const warpsim::LPId id = static_cast<warpsim::LPId>(1 + i);
        if (owns_lp(id))
        {
            auto lp = std::make_unique<AirportLP>(id, p);
            airports.push_back(lp.get());
            sim.add_lp(std::move(lp));
            ++localAirportCount;
        }
    }

    // Driver seeds initial arrivals.
    if (owns_lp(driverId))
    {
        sim.add_lp(std::make_unique<DriverLP>(driverId, p));
        hasDriver = true;
    }

    if (debug)
    {
        std::cerr << "[airport_fujimoto][rank=" << rank << "/" << size << "] localAirports=" << localAirportCount
                  << " hasDriver=" << (hasDriver ? 1 : 0) << "\n";
    }

    sim.run();

    std::uint64_t globalHash = committedHash;
    std::uint64_t totalArrivals = 0;
    std::uint64_t totalDepartures = 0;
    std::uint64_t maxQ = 0;

    for (const auto *ap : airports)
    {
        totalArrivals += ap->arrivals();
        totalDepartures += ap->departures();
        if (ap->max_queue() > maxQ)
        {
            maxQ = ap->max_queue();
        }
    }

    if (debug)
    {
        std::cerr << "[airport_fujimoto][rank=" << rank << "/" << size << "] localHash=" << committedHash
                  << " localArrivals=" << totalArrivals
                  << " localDepartures=" << totalDepartures
                  << " localMaxQ=" << maxQ << "\n";

        const auto st = sim.stats();
        std::cerr << "[airport_fujimoto][rank=" << rank << "/" << size << "] stats: pending=" << st.pending
                  << " inflight=" << st.inflight
                  << " cancelled=" << st.cancelled
                  << " processed=" << st.processed
                  << " totalProcessed=" << st.totalProcessed
                  << " rollbacksTotal=" << st.rollbacksTotal
                  << " rollbacksStraggler=" << st.rollbacksStraggler
                  << " rollbacksAnti=" << st.rollbacksAnti
                  << " antiCancelledPending=" << st.antiCancelledPending
                  << " antiCancelledProcessed=" << st.antiCancelledProcessed
                  << " receivedEvents=" << st.receivedEvents
                  << " receivedAnti=" << st.receivedAnti
                  << " sentEvents=" << st.sentEvents
                  << " sentAnti=" << st.sentAnti
                  << " gvt=" << st.gvt.time << "." << st.gvt.sequence
                  << "\n";
    }

#if defined(WARPSIM_HAS_MPI)
    if (size > 1)
    {
        MPI_Bcast(&globalHash, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
        MPI_Bcast(&totalArrivals, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
        MPI_Bcast(&totalDepartures, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
        MPI_Bcast(&maxQ, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
    }
#endif

    if (rank == 0)
    {
        std::cout << "airport_fujimoto hash=" << globalHash
                  << " arrivals=" << totalArrivals
                  << " departures=" << totalDepartures
                  << " maxQ=" << maxQ
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
                std::cerr << "airport_fujimoto verify failed: expected=" << p.expectedHash << " got=" << globalHash << "\n";
            }
#if defined(WARPSIM_HAS_MPI)
            MPI_Finalize();
#endif
            return 3;
        }

        if (rank == 0)
        {
            std::cout << "airport_fujimoto ok\n";
        }
    }

#if defined(WARPSIM_HAS_MPI)
    MPI_Finalize();
#endif
    return 0;
}
