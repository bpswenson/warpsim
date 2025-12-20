#include "random.hpp"
#include "simulation.hpp"
#include "transport.hpp"

#if defined(WARPSIM_HAS_MPI)
#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include <mpi.h>
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace
{
    using EventKind = std::uint32_t;

    enum : EventKind
    {
        KindPulse = 8001,
        KindCommittedPulse = 8101,
    };

    struct Params
    {
        std::uint32_t lps = 1024;
        std::uint32_t initEventsPerLp = 1;
        std::uint64_t endTime = 10000;
        double remoteFraction = 0.9;
        double meanLookahead = 5.0;
        std::uint64_t seed = 1;

        // If non-zero, emit committed records only when (logicalKey % emitMod == 0).
        std::uint32_t emitMod = 0;

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

    [[noreturn]] void usage_and_exit(int rank)
    {
        if (rank == 0)
        {
            std::cerr << "PHOLD (determinism oracle)\n"
                      << "  --lps N\n"
                      << "  --init K\n"
                      << "  --end T\n"
                      << "  --remote P\n"
                      << "  --lookahead MEAN\n"
                      << "  --seed S\n"
                      << "  --emit-mod M\n"
                      << "  --verify\n"
                      << "  --expected-hash H\n";
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

            if (a == "--lps")
            {
                if (!parse_u32(need(), p.lps))
                    usage_and_exit(rank);
            }
            else if (a == "--init")
            {
                if (!parse_u32(need(), p.initEventsPerLp))
                    usage_and_exit(rank);
            }
            else if (a == "--end")
            {
                if (!parse_u64(need(), p.endTime))
                    usage_and_exit(rank);
            }
            else if (a == "--remote")
            {
                if (!parse_double(need(), p.remoteFraction))
                    usage_and_exit(rank);
            }
            else if (a == "--lookahead")
            {
                if (!parse_double(need(), p.meanLookahead))
                    usage_and_exit(rank);
            }
            else if (a == "--seed")
            {
                if (!parse_u64(need(), p.seed))
                    usage_and_exit(rank);
            }
            else if (a == "--emit-mod")
            {
                if (!parse_u32(need(), p.emitMod))
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

        if (p.lps == 0 || p.meanLookahead <= 0.0 || p.remoteFraction < 0.0 || p.remoteFraction > 1.0)
        {
            usage_and_exit(rank);
        }
        return p;
    }

    inline warpsim::EventUid next_logical_key(std::uint64_t seed,
                                              warpsim::LPId src,
                                              warpsim::LPId dst,
                                              warpsim::EventUid prev,
                                              std::uint64_t nextTime) noexcept
    {
        std::uint64_t x = seed;
        x = warpsim::mix_u64(x, static_cast<std::uint64_t>(src));
        x = warpsim::mix_u64(x, static_cast<std::uint64_t>(dst));
        x = warpsim::mix_u64(x, static_cast<std::uint64_t>(prev));
        x = warpsim::mix_u64(x, nextTime);
        return static_cast<warpsim::EventUid>(warpsim::splitmix64(x));
    }

    inline std::uint64_t stable_seq(warpsim::LPId dst, warpsim::EventUid logicalKey) noexcept
    {
        // Sequence must be stable across MPI rank counts.
        // Keep it non-zero to avoid clashing with “default 0” events.
        return warpsim::splitmix64(static_cast<std::uint64_t>(dst) ^ warpsim::splitmix64(static_cast<std::uint64_t>(logicalKey))) | 1ULL;
    }

    struct PulseArgs
    {
        warpsim::EventUid logicalKey = 0;
    };

    struct CommitPulse
    {
        std::uint32_t src = 0;
        std::uint32_t dst = 0;
        std::uint64_t t = 0;
        warpsim::EventUid logicalKey = 0;
    };

    class PholdLP final : public warpsim::ILogicalProcess
    {
    public:
        PholdLP(warpsim::LPId id, Params params) : m_id(id), m_params(params) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            for (std::uint32_t k = 0; k < m_params.initEventsPerLp; ++k)
            {
                const warpsim::EventUid key = next_logical_key(m_params.seed, m_id, m_id, /*prev=*/k, /*nextTime=*/0);

                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{0, stable_seq(m_id, key)};
                ev.src = m_id;
                ev.dst = m_id;
                ev.payload.kind = KindPulse;
                ev.payload.bytes = warpsim::bytes_from_trivially_copyable(PulseArgs{.logicalKey = key});
                sink.send(std::move(ev));
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind != KindPulse || ev.payload.bytes.size() != sizeof(PulseArgs))
            {
                throw std::runtime_error("phold: unexpected payload");
            }

            PulseArgs args{};
            std::memcpy(&args, ev.payload.bytes.data(), sizeof(args));
            const warpsim::EventUid key = args.logicalKey;

            if (ev.ts.time >= m_params.endTime)
            {
                return;
            }

            warpsim::LPId dst = m_id;
            const double uRemote = warpsim::rng_unit_double(m_params.seed, m_id, /*stream=*/1, key, /*draw=*/0);
            if (uRemote < m_params.remoteFraction)
            {
                const std::uint64_t r = warpsim::rng_u64_range(m_params.seed, m_id, /*stream=*/2, key,
                                                               /*lo=*/0, /*hi=*/m_params.lps - 1, /*draw=*/0);
                dst = static_cast<warpsim::LPId>(r);
                if (m_params.lps > 1 && dst == m_id)
                {
                    dst = static_cast<warpsim::LPId>((dst + 1) % m_params.lps);
                }
            }

            double u = warpsim::rng_unit_double(m_params.seed, m_id, /*stream=*/3, key, /*draw=*/0);
            u = std::max(u, std::numeric_limits<double>::min());
            const std::uint64_t delta = 1 + static_cast<std::uint64_t>(std::floor(-m_params.meanLookahead * std::log(u)));

            const std::uint64_t nextTime = ev.ts.time + delta;
            if (nextTime > m_params.endTime)
            {
                return;
            }

            if (m_params.emitMod == 0 || (static_cast<std::uint64_t>(key) % m_params.emitMod) == 0)
            {
                CommitPulse rec;
                rec.src = static_cast<std::uint32_t>(m_id);
                rec.dst = static_cast<std::uint32_t>(dst);
                rec.t = ev.ts.time;
                rec.logicalKey = key;

                warpsim::Payload out;
                out.kind = KindCommittedPulse;
                out.bytes = warpsim::bytes_from_trivially_copyable(rec);
                ctx.emit_committed(ev.ts, std::move(out));
            }

            const warpsim::EventUid nextKey = next_logical_key(m_params.seed, m_id, dst, key, nextTime);

            warpsim::Event next;
            next.ts = warpsim::TimeStamp{nextTime, stable_seq(dst, nextKey)};
            next.src = m_id;
            next.dst = dst;
            next.payload.kind = KindPulse;
            next.payload.bytes = warpsim::bytes_from_trivially_copyable(PulseArgs{.logicalKey = nextKey});
            ctx.send(std::move(next));
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        Params m_params;
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
    cfg.collectivePeriodIters = 256;

#if defined(WARPSIM_HAS_MPI)
    if (size > 1)
    {
        cfg.lpToRank = [size](warpsim::LPId lp) -> warpsim::RankId
        { return static_cast<warpsim::RankId>(lp % static_cast<warpsim::LPId>(size)); };
        cfg.gvtReduceMin = [=](warpsim::TimeStamp local)
        { return warpsim::mpi_allreduce_min_timestamp(MPI_COMM_WORLD, local); };
        cfg.anyRankHasWork = [=](bool local)
        { return warpsim::mpi_allreduce_any_work(MPI_COMM_WORLD, local); };
    }
#endif

    std::uint64_t localHash = 0;
    cfg.committedSink = [&](warpsim::RankId, warpsim::LPId, warpsim::TimeStamp, const warpsim::Payload &payload)
    {
        if (payload.kind != KindCommittedPulse)
        {
            return;
        }
        if (payload.bytes.size() != sizeof(CommitPulse))
        {
            throw std::runtime_error("phold: bad committed payload");
        }

        CommitPulse rec{};
        std::memcpy(&rec, payload.bytes.data(), sizeof(rec));

        std::uint64_t x = 0;
        x = warpsim::mix_u64(x, rec.src);
        x = warpsim::mix_u64(x, rec.dst);
        x = warpsim::mix_u64(x, rec.t);
        x = warpsim::mix_u64(x, static_cast<std::uint64_t>(rec.logicalKey));
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

    for (std::uint32_t i = 0; i < p.lps; ++i)
    {
        const warpsim::LPId id = static_cast<warpsim::LPId>(i);
        if (owns_lp(id))
        {
            sim.add_lp(std::make_unique<PholdLP>(id, p));
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
        std::cout << "phold hash=" << globalHash << "\n";
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
                std::cerr << "phold verify failed: expected=" << p.expectedHash << " got=" << globalHash << "\n";
            }
            exitCode = 3;
        }
    }

#if defined(WARPSIM_HAS_MPI)
    MPI_Finalize();
#endif
    return exitCode;
}
