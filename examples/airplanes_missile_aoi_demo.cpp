#include "aoi_client.hpp"
#include "aoi_lp.hpp"
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

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    using EventKind = std::uint32_t;

    enum : EventKind
    {
        // AOI protocol kinds for this demo.
        Aoi_Update = 72001,
        Aoi_Query = 72002,
        Aoi_Result = 72003,

        // Model events.
        Region_Tick = 72101,
        Missile_Hit = 72201,

        // Committed output.
        Commit_Hit = 72301,
    };

    struct Params
    {
        std::uint32_t regions = 8;
        std::uint32_t planesPerRegion = 64;
        std::uint64_t endTime = 400;

        double regionSize = 1000.0;
        double speed = 12.0;
        double detectRadius = 650.0;
        std::uint32_t queryEvery = 5;
        std::uint32_t damage = 1;

        std::uint64_t seed = 1;

        bool verify = false;
        bool hasExpected = false;
        std::uint64_t expectedHash = 0;
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

    bool parse_double(std::string_view s, double &out)
    {
        try
        {
            std::string tmp(s);
            size_t idx = 0;
            out = std::stod(tmp, &idx);
            return idx == tmp.size();
        }
        catch (...)
        {
            return false;
        }
    }

#if defined(WARPSIM_HAS_MPI)
    [[noreturn]] void usage_and_exit(int rank)
#else
    [[noreturn]] void usage_and_exit(int)
#endif
    {
#if defined(WARPSIM_HAS_MPI)
        if (rank == 0)
#endif
        {
            std::cerr << "Airplanes + AOI + missiles demo\n"
                      << "  --regions N\n"
                      << "  --planes-per-region N\n"
                      << "  --end T\n"
                      << "  --seed S\n"
                      << "  --region-size X\n"
                      << "  --speed V\n"
                      << "  --detect-radius R\n"
                      << "  --query-every K\n"
                      << "  --damage D\n"
                      << "  --verify --expected-hash H\n";
        }
#if defined(WARPSIM_HAS_MPI)
        MPI_Abort(MPI_COMM_WORLD, 2);
#endif
        std::exit(2);
    }

    Params parse_args(int argc, char **argv, int rank)
    {
        Params p;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view a(argv[i]);
            auto need = [&]() -> std::string_view
            {
                if (i + 1 >= argc)
                {
                    usage_and_exit(rank);
                }
                return std::string_view(argv[++i]);
            };

            if (a == "--regions")
            {
                if (!parse_u32(need(), p.regions))
                    usage_and_exit(rank);
            }
            else if (a == "--planes-per-region")
            {
                if (!parse_u32(need(), p.planesPerRegion))
                    usage_and_exit(rank);
            }
            else if (a == "--end")
            {
                if (!parse_u64(need(), p.endTime))
                    usage_and_exit(rank);
            }
            else if (a == "--seed")
            {
                if (!parse_u64(need(), p.seed))
                    usage_and_exit(rank);
            }
            else if (a == "--region-size")
            {
                if (!parse_double(need(), p.regionSize))
                    usage_and_exit(rank);
            }
            else if (a == "--speed")
            {
                if (!parse_double(need(), p.speed))
                    usage_and_exit(rank);
            }
            else if (a == "--detect-radius")
            {
                if (!parse_double(need(), p.detectRadius))
                    usage_and_exit(rank);
            }
            else if (a == "--query-every")
            {
                if (!parse_u32(need(), p.queryEvery))
                    usage_and_exit(rank);
            }
            else if (a == "--damage")
            {
                if (!parse_u32(need(), p.damage))
                    usage_and_exit(rank);
            }
            else if (a == "--verify")
            {
                p.verify = true;
            }
            else if (a == "--expected-hash")
            {
                p.hasExpected = true;
                if (!parse_u64(need(), p.expectedHash))
                    usage_and_exit(rank);
            }
            else
            {
                usage_and_exit(rank);
            }
        }

        if (p.regions < 2 || p.planesPerRegion < 2)
        {
            usage_and_exit(rank);
        }
        if (p.queryEvery == 0)
        {
            usage_and_exit(rank);
        }
        return p;
    }

    static constexpr warpsim::EntityId make_plane_id(warpsim::LPId owner, std::uint32_t local)
    {
        return (static_cast<warpsim::EntityId>(owner) << 32) | static_cast<warpsim::EntityId>(local);
    }

    static constexpr warpsim::LPId owner_lp(warpsim::EntityId id)
    {
        return static_cast<warpsim::LPId>(id >> 32);
    }

    struct Plane
    {
        warpsim::Vec3 pos{};
        warpsim::Vec3 vel{};
        std::uint32_t hp = 3;

        std::uint64_t lastFireTime = 0;
        std::uint64_t shotsFired = 0;
        std::uint64_t hitsTaken = 0;
        std::uint64_t kills = 0;

        std::uint8_t alive = 1;
    };

    static inline void write_f64(warpsim::WireWriter &w, double v)
    {
        const std::uint64_t bits = std::bit_cast<std::uint64_t>(v);
        w.write_u64(bits);
    }

    static inline double read_f64(warpsim::WireReader &r)
    {
        const std::uint64_t bits = r.read_u64();
        return std::bit_cast<double>(bits);
    }

    static inline void write_vec3(warpsim::WireWriter &w, const warpsim::Vec3 &v)
    {
        write_f64(w, v.x);
        write_f64(w, v.y);
        write_f64(w, v.z);
    }

    static inline warpsim::Vec3 read_vec3(warpsim::WireReader &r)
    {
        warpsim::Vec3 v{};
        v.x = read_f64(r);
        v.y = read_f64(r);
        v.z = read_f64(r);
        return v;
    }

    static inline warpsim::ByteBuffer encode_plane(const Plane &p)
    {
        warpsim::WireWriter w;
        write_vec3(w, p.pos);
        write_vec3(w, p.vel);
        w.write_u32(p.hp);
        w.write_u64(p.lastFireTime);
        w.write_u64(p.shotsFired);
        w.write_u64(p.hitsTaken);
        w.write_u64(p.kills);
        w.write_u8(p.alive);
        return w.take();
    }

    static inline Plane decode_plane(std::span<const std::byte> bytes)
    {
        warpsim::WireReader r(bytes);
        Plane p{};
        p.pos = read_vec3(r);
        p.vel = read_vec3(r);
        p.hp = r.read_u32();
        p.lastFireTime = r.read_u64();
        p.shotsFired = r.read_u64();
        p.hitsTaken = r.read_u64();
        p.kills = r.read_u64();
        p.alive = r.read_u8();
        return p;
    }

    static inline warpsim::ByteBuffer encode_hit_args(warpsim::EntityId shooter, warpsim::EntityId target, std::uint32_t damage)
    {
        warpsim::WireWriter w;
        w.write_u64(static_cast<std::uint64_t>(shooter));
        w.write_u64(static_cast<std::uint64_t>(target));
        w.write_u32(damage);
        return w.take();
    }

    struct HitArgsDecoded
    {
        warpsim::EntityId shooter = 0;
        warpsim::EntityId target = 0;
        std::uint32_t damage = 1;
    };

    static inline HitArgsDecoded decode_hit_args(std::span<const std::byte> bytes)
    {
        warpsim::WireReader r(bytes);
        HitArgsDecoded a{};
        a.shooter = static_cast<warpsim::EntityId>(r.read_u64());
        a.target = static_cast<warpsim::EntityId>(r.read_u64());
        a.damage = r.read_u32();
        return a;
    }

    static inline warpsim::ByteBuffer encode_hit_commit(warpsim::EntityId shooter, warpsim::EntityId target, std::uint64_t t, bool killed)
    {
        warpsim::WireWriter w;
        w.write_u64(static_cast<std::uint64_t>(shooter));
        w.write_u64(static_cast<std::uint64_t>(target));
        w.write_u64(t);
        w.write_u8(static_cast<std::uint8_t>(killed ? 1 : 0));
        return w.take();
    }

    struct HitCommitDecoded
    {
        warpsim::EntityId shooter = 0;
        warpsim::EntityId target = 0;
        std::uint64_t t = 0;
        std::uint8_t killed = 0;
    };

    static inline HitCommitDecoded decode_hit_commit(std::span<const std::byte> bytes)
    {
        warpsim::WireReader r(bytes);
        HitCommitDecoded c{};
        c.shooter = static_cast<warpsim::EntityId>(r.read_u64());
        c.target = static_cast<warpsim::EntityId>(r.read_u64());
        c.t = r.read_u64();
        c.killed = r.read_u8();
        return c;
    }

    class RegionLP final : public warpsim::ILogicalProcess
    {
    public:
        RegionLP(warpsim::LPId id, warpsim::LPId aoiLp, Params p)
            : m_id(id), m_aoiLp(aoiLp), m_params(std::move(p)), m_world(m_registry)
        {
            m_aoiClient.aoiLp = m_aoiLp;
            m_aoiClient.updateKind = Aoi_Update;
            m_aoiClient.queryKind = Aoi_Query;

            // Avoid padding/UB issues from raw memcpy snapshotting: serialize/deserialize
            // planes explicitly so rollback and hashing are stable across platforms and ranks.
            warpsim::EntityTypeOps ops;
            ops.type = kPlaneType;
            ops.create = []() -> void *
            { return new Plane{}; };
            ops.destroy = [](void *p)
            { delete static_cast<Plane *>(p); };
            ops.serialize = [](const void *p) -> warpsim::ByteBuffer
            {
                return encode_plane(*static_cast<const Plane *>(p));
            };
            ops.deserialize = [](void *p, std::span<const std::byte> bytes)
            {
                *static_cast<Plane *>(p) = decode_plane(bytes);
            };
            m_registry.register_type(std::move(ops));

            m_planeIds.reserve(m_params.planesPerRegion);
            for (std::uint32_t i = 1; i <= m_params.planesPerRegion; ++i)
            {
                const warpsim::EntityId id = make_plane_id(m_id, i);
                Plane pl{};

                // Make plane 1 in each region deterministic and near region boundaries to
                // encourage cross-LP contacts and missile shots.
                const double baseX = (static_cast<double>(m_id) - 1.0) * m_params.regionSize;
                if (i == 1)
                {
                    pl.pos = warpsim::Vec3{baseX + m_params.regionSize - 10.0, 0.5 * m_params.regionSize, 0.0};
                    pl.vel = warpsim::Vec3{m_params.speed, 0.0, 0.0};
                }
                else if (i == 2)
                {
                    pl.pos = warpsim::Vec3{baseX + 10.0, 0.5 * m_params.regionSize, 0.0};
                    pl.vel = warpsim::Vec3{-m_params.speed, 0.0, 0.0};
                }
                else
                {
                    const double ux = warpsim::rng_unit_double(m_params.seed, m_id, /*stream=*/1, m_id, /*uid=*/i, /*draw=*/0);
                    const double uy = warpsim::rng_unit_double(m_params.seed, m_id, /*stream=*/2, m_id, /*uid=*/i, /*draw=*/0);
                    const double ua = warpsim::rng_unit_double(m_params.seed, m_id, /*stream=*/3, m_id, /*uid=*/i, /*draw=*/0);

                    pl.pos.x = baseX + ux * m_params.regionSize;
                    pl.pos.y = uy * m_params.regionSize;
                    pl.pos.z = 0;

                    const double ang = ua * 2.0 * 3.141592653589793;
                    pl.vel.x = std::cos(ang) * m_params.speed;
                    pl.vel.y = std::sin(ang) * m_params.speed;
                    pl.vel.z = 0;
                }

                m_world.emplace<Plane>(id, kPlaneType, pl);
                m_planeIds.push_back(id);
            }
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        bool supports_entity_snapshots() const override { return true; }

        warpsim::ByteBuffer save_entity(warpsim::EntityId id) const override
        {
            return m_world.serialize_entity(id);
        }

        void load_entity(warpsim::EntityId id, std::span<const std::byte> bytes) override
        {
            m_world.deserialize_entity(id, bytes);
        }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Seed initial AOI positions.
            for (const warpsim::EntityId pid : m_planeIds)
            {
                const auto &pl = m_world.read<Plane>(pid, kPlaneType);
                m_aoiClient.send_update(sink, warpsim::TimeStamp{0, 0}, m_id, pid, pl.pos);
            }

            // Drive local time forward conservatively: schedule the next tick only after
            // any AOI query result for the current tick has been received. This keeps the
            // demo from sprinting far ahead and then rolling back heavily under MPI.
            warpsim::Event tick;
            tick.ts = warpsim::TimeStamp{1, 0};
            tick.src = m_id;
            tick.dst = m_id;
            tick.payload.kind = Region_Tick;
            sink.send(std::move(tick));
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == Region_Tick)
            {
                on_tick_(ev.ts, ctx);
                return;
            }

            if (ev.payload.kind == Aoi_Result)
            {
                on_aoi_result_(ev, ctx);
                return;
            }

            if (ev.payload.kind == Missile_Hit)
            {
                on_missile_hit_(ev, ctx);
                return;
            }
        }

        warpsim::ByteBuffer save_state() const override
        {
            // Even with per-entity snapshots enabled, we must snapshot any additional
            // LP-local control state that affects behavior under rollback.
            return warpsim::bytes_from_trivially_copyable(m_waitingReqId);
        }

        void load_state(std::span<const std::byte> bytes) override
        {
            if (bytes.size() != sizeof(m_waitingReqId))
            {
                m_waitingReqId = 0;
                return;
            }
            std::memcpy(&m_waitingReqId, bytes.data(), sizeof(m_waitingReqId));
        }

    private:
        static constexpr warpsim::EntityTypeId kPlaneType = 1;

        static constexpr std::uint8_t kReqTagRadar = 1;

        void on_tick_(warpsim::TimeStamp ts, warpsim::IEventContext &ctx)
        {
            if (ts.time == 0 || ts.time > m_params.endTime)
            {
                return;
            }

            warpsim::WorldTxn txn(m_world, ctx);

            const double baseX = (static_cast<double>(m_id) - 1.0) * m_params.regionSize;
            const double minX = baseX;
            const double maxX = baseX + m_params.regionSize;
            const double minY = 0.0;
            const double maxY = m_params.regionSize;

            const std::uint64_t t = ts.time;

            // Update all planes (entity snapshots via WorldTxn::write -> ctx.request_write).
            for (const warpsim::EntityId pid : m_planeIds)
            {
                auto &pl = txn.write<Plane>(pid, kPlaneType);
                if (!pl.alive)
                {
                    continue;
                }

                pl.pos.x += pl.vel.x;
                pl.pos.y += pl.vel.y;

                // Bounce within region bounds.
                if (pl.pos.x < minX)
                {
                    pl.pos.x = minX + (minX - pl.pos.x);
                    pl.vel.x = -pl.vel.x;
                }
                if (pl.pos.x > maxX)
                {
                    pl.pos.x = maxX - (pl.pos.x - maxX);
                    pl.vel.x = -pl.vel.x;
                }
                if (pl.pos.y < minY)
                {
                    pl.pos.y = minY + (minY - pl.pos.y);
                    pl.vel.y = -pl.vel.y;
                }
                if (pl.pos.y > maxY)
                {
                    pl.pos.y = maxY - (pl.pos.y - maxY);
                    pl.vel.y = -pl.vel.y;
                }

                m_aoiClient.send_update(ctx, ts, m_id, pid, pl.pos);
            }

            // Optional radar query + missile shot: at most one query per tick.
            // This keeps MPI runs responsive and avoids pathological rollback storms
            // from same-timestamp cross-rank query/response traffic.
            if ((t % m_params.queryEvery) == 0 && m_waitingReqId == 0)
            {
                const std::size_t idx = static_cast<std::size_t>((t / m_params.queryEvery) % std::max<std::uint32_t>(1u, m_params.planesPerRegion));
                const warpsim::EntityId shooter = m_planeIds[idx];
                const auto &pl = m_world.read<Plane>(shooter, kPlaneType);
                if (pl.alive && pl.lastFireTime + 1 <= t)
                {
                    // Deterministic requestId allows mapping results back to (LP, plane).
                    const std::uint64_t req = (static_cast<std::uint64_t>(kReqTagRadar) << 56) |
                                              ((static_cast<std::uint64_t>(m_id) & 0x00FFFFFFULL) << 32) |
                                              ((static_cast<std::uint64_t>(shooter & 0xFFFFFFFFULL) & 0xFFFFULL) << 16) |
                                              (t & 0xFFFFULL);

                    warpsim::AoiLP::QueryArgs q;
                    q.requestId = req;
                    q.self = shooter;
                    q.shape = warpsim::AoiLP::QueryShape::Sphere;
                    q.origin = pl.pos;
                    q.radius = m_params.detectRadius;

                    m_waitingReqId = req;
                    m_aoiClient.send_query(ctx, ts, m_id, q);
                }
            }

            // Advance to next tick regardless of whether a query was sent.
            if (t + 1 <= m_params.endTime)
            {
                warpsim::Event next;
                next.ts = warpsim::TimeStamp{t + 1, 0};
                next.src = m_id;
                next.dst = m_id;
                next.payload.kind = Region_Tick;
                ctx.send(std::move(next));
            }
        }

        void on_aoi_result_(const warpsim::Event &ev, warpsim::IEventContext &ctx)
        {
            const auto res = warpsim::AoiClient::decode_result(ev.payload);

            if (m_waitingReqId == 0 || res.requestId != m_waitingReqId)
            {
                return;
            }
            m_waitingReqId = 0;

            const std::uint8_t tag = static_cast<std::uint8_t>(res.requestId >> 56);
            if (tag != kReqTagRadar)
            {
                return;
            }

            const warpsim::LPId shooterLp = static_cast<warpsim::LPId>((res.requestId >> 32) & 0x00FFFFFFULL);
            const std::uint32_t shooterLocal = static_cast<std::uint32_t>((res.requestId >> 16) & 0xFFFFULL);
            const warpsim::EntityId shooter = make_plane_id(shooterLp, shooterLocal);

            if (shooterLp != m_id)
            {
                // This result is for a different LP; ignore.
                return;
            }

            // Pick a target on a different LP if possible.
            warpsim::EntityId target = 0;
            for (const auto e : res.entities)
            {
                if (owner_lp(e) != shooterLp)
                {
                    target = e;
                    break;
                }
            }
            if (target == 0)
            {
                return;
            }

            // Mark that this plane fired (entity snapshot).
            {
                warpsim::WorldTxn txn(m_world, ctx);
                auto &pl = txn.write<Plane>(shooter, kPlaneType);
                if (!pl.alive)
                {
                    return;
                }
                pl.lastFireTime = ev.ts.time;
                ++pl.shotsFired;
            }

            warpsim::Event hit;
            hit.ts = warpsim::TimeStamp{ev.ts.time + 1, 0};
            hit.src = m_id;
            hit.dst = owner_lp(target);
            hit.target = target;
            hit.payload.kind = Missile_Hit;
            hit.payload.bytes = encode_hit_args(shooter, target, m_params.damage);
            ctx.send(std::move(hit));
        }

        void on_missile_hit_(const warpsim::Event &ev, warpsim::IEventContext &ctx)
        {
            const HitArgsDecoded a = decode_hit_args(std::span<const std::byte>(ev.payload.bytes.data(), ev.payload.bytes.size()));

            if (a.target == 0)
            {
                return;
            }

            warpsim::WorldTxn txn(m_world, ctx);
            auto &tgt = txn.write<Plane>(a.target, kPlaneType);
            if (!tgt.alive)
            {
                return;
            }

            ++tgt.hitsTaken;
            const std::uint32_t dmg = (a.damage == 0) ? 1u : a.damage;
            if (tgt.hp > dmg)
            {
                tgt.hp -= dmg;
            }
            else
            {
                tgt.hp = 0;
                tgt.alive = 0;
                // Move far away so AOI won't keep returning this plane.
                tgt.pos = warpsim::Vec3{1e9, 1e9, 1e9};
                m_aoiClient.send_update(ctx, ev.ts, m_id, a.target, tgt.pos);
            }

            // Bookkeeping on shooter if it's local to this LP.
            if (owner_lp(a.shooter) == m_id)
            {
                auto &sh = txn.write<Plane>(a.shooter, kPlaneType);
                if (tgt.alive == 0)
                {
                    ++sh.kills;
                }
            }

            // Emit deterministic committed output for verification/hashing.
            warpsim::Payload out;
            out.kind = Commit_Hit;
            out.bytes = encode_hit_commit(a.shooter, a.target, ev.ts.time, tgt.alive == 0);
            ctx.emit_committed(ev.ts, std::move(out));
        }

        warpsim::LPId m_id = 0;
        warpsim::LPId m_aoiLp = 0;
        Params m_params;

        std::uint64_t m_waitingReqId = 0;

        warpsim::AoiClient m_aoiClient;
        warpsim::EntityTypeRegistry m_registry;
        warpsim::World m_world;
        std::vector<warpsim::EntityId> m_planeIds;
    };
}

int main(int argc, char **argv)
{
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

    const Params p = parse_args(argc, argv, rank);

    // Keep AOI placement stable across different MPI sizes.
    const warpsim::LPId aoiId = 10000;

    std::shared_ptr<warpsim::ITransport> transport;
#if defined(WARPSIM_HAS_MPI)
    if (size > 1)
    {
        transport = std::make_shared<warpsim::MpiTransport>(MPI_COMM_WORLD, /*tag=*/11);
    }
    else
#endif
    {
        transport = std::make_shared<warpsim::InProcTransport>();
    }

    warpsim::SimulationConfig cfg;
    cfg.rank = static_cast<warpsim::RankId>(rank);
    cfg.seed = p.seed;
    cfg.collectivePeriodIters = 128;

#if defined(WARPSIM_HAS_MPI)
    if (size > 1)
    {
        cfg.lpToRank = [size, aoiId](warpsim::LPId lp) -> warpsim::RankId
        {
            // Pin AOI to rank 0 so behavior is less sensitive to size.
            if (lp == aoiId)
            {
                return 0;
            }
            return static_cast<warpsim::RankId>(lp % static_cast<warpsim::LPId>(size));
        };
        cfg.gvtReduceMin = [=](warpsim::TimeStamp local)
        { return warpsim::mpi_allreduce_min_timestamp(MPI_COMM_WORLD, local); };
        cfg.anyRankHasWork = [=](bool local)
        { return warpsim::mpi_allreduce_any_work(MPI_COMM_WORLD, local); };
    }
#endif

    std::uint64_t localHash = 0;
    cfg.committedSink = [&](warpsim::RankId, warpsim::LPId, warpsim::TimeStamp, const warpsim::Payload &payload)
    {
        if (payload.kind != Commit_Hit)
        {
            return;
        }
        const auto rec = decode_hit_commit(std::span<const std::byte>(payload.bytes.data(), payload.bytes.size()));

        std::uint64_t x = 0;
        x = warpsim::mix_u64(x, static_cast<std::uint64_t>(rec.shooter));
        x = warpsim::mix_u64(x, static_cast<std::uint64_t>(rec.target));
        x = warpsim::mix_u64(x, rec.t);
        x = warpsim::mix_u64(x, static_cast<std::uint64_t>(rec.killed));
        localHash ^= warpsim::splitmix64(x);
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

    // AOI LP.
    if (owns_lp(aoiId))
    {
        warpsim::AoiLP::Config aoiCfg;
        aoiCfg.id = aoiId;
        aoiCfg.stateEntityId = 0xA01A01ULL;
        aoiCfg.updateKind = Aoi_Update;
        aoiCfg.queryKind = Aoi_Query;
        aoiCfg.resultKind = Aoi_Result;
        sim.add_lp(std::make_unique<warpsim::AoiLP>(aoiCfg));
    }

    // Region LPs: ids [1..regions].
    for (std::uint32_t i = 1; i <= p.regions; ++i)
    {
        const warpsim::LPId id = static_cast<warpsim::LPId>(i);
        if (owns_lp(id))
        {
            sim.add_lp(std::make_unique<RegionLP>(id, aoiId, p));
        }
    }

    sim.run();

    std::uint64_t globalHash = localHash;
#if defined(WARPSIM_HAS_MPI)
    if (size > 1)
    {
        std::uint64_t out = 0;
        MPI_Allreduce(&globalHash, &out, 1, MPI_UINT64_T, MPI_BXOR, MPI_COMM_WORLD);
        globalHash = out;
    }
#endif

    if (rank == 0)
    {
        std::cout << "airplanes_missile_aoi_demo hash=" << globalHash << "\n";
    }

    int exitCode = 0;
    if (p.verify)
    {
        if (!p.hasExpected)
        {
            if (rank == 0)
            {
                std::cerr << "--verify requires --expected-hash\n";
            }
            exitCode = 2;
        }
        else if (globalHash != p.expectedHash)
        {
            if (rank == 0)
            {
                std::cerr << "hash mismatch: got=" << globalHash << " expected=" << p.expectedHash << "\n";
            }
            exitCode = 2;
        }
    }

#if defined(WARPSIM_HAS_MPI)
    MPI_Finalize();
#endif

    return exitCode;
}
