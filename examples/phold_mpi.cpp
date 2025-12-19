#include "mpi_transport.hpp"
#include "mpi_collectives.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

namespace
{
    struct Params
    {
        std::uint32_t lps = 1024;
        std::uint32_t initEventsPerLp = 1;
        std::uint64_t endTime = 10000;
        double remoteFraction = 0.9;
        double meanLookahead = 5.0;
        std::uint64_t seed = 1;
    };

    bool parse_u32(std::string_view s, std::uint32_t &out)
    {
        const char *b = s.data();
        const char *e = s.data() + s.size();
        unsigned long v = 0;
        auto r = std::from_chars(b, e, v);
        if (r.ec != std::errc() || r.ptr != e || v > std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }
        out = static_cast<std::uint32_t>(v);
        return true;
    }

    bool parse_u64(std::string_view s, std::uint64_t &out)
    {
        const char *b = s.data();
        const char *e = s.data() + s.size();
        unsigned long long v = 0;
        auto r = std::from_chars(b, e, v);
        if (r.ec != std::errc() || r.ptr != e)
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
            std::cerr << "PHOLD MPI benchmark\n"
                      << "  --lps N                 total LPs/entities (default 1024)\n"
                      << "  --init K                initial events per LP (default 1)\n"
                      << "  --end T                 stop generating at sim time T (default 10000)\n"
                      << "  --remote P              remote fraction [0..1] (default 0.9)\n"
                      << "  --lookahead MEAN        mean timestamp increment (default 5)\n"
                      << "  --seed S                base seed (default 1)\n";
        }
        MPI_Abort(MPI_COMM_WORLD, 2);
        std::abort();
    }

    Params parse_args(int argc, char **argv, int rank)
    {
        Params p;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view a(argv[i]);
            auto need = [&](std::string_view flag) -> std::string_view
            {
                if (i + 1 >= argc)
                {
                    usage_and_exit(rank);
                }
                ++i;
                (void)flag;
                return std::string_view(argv[i]);
            };

            if (a == "--lps")
            {
                if (!parse_u32(need(a), p.lps))
                    usage_and_exit(rank);
            }
            else if (a == "--init")
            {
                if (!parse_u32(need(a), p.initEventsPerLp))
                    usage_and_exit(rank);
            }
            else if (a == "--end")
            {
                if (!parse_u64(need(a), p.endTime))
                    usage_and_exit(rank);
            }
            else if (a == "--remote")
            {
                if (!parse_double(need(a), p.remoteFraction))
                    usage_and_exit(rank);
            }
            else if (a == "--lookahead")
            {
                if (!parse_double(need(a), p.meanLookahead))
                    usage_and_exit(rank);
            }
            else if (a == "--seed")
            {
                if (!parse_u64(need(a), p.seed))
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

    // splitmix64 (deterministic stateless RNG) for rollback-safe pseudo-randomness.
    inline std::uint64_t splitmix64(std::uint64_t x)
    {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    inline double u01_from_u64(std::uint64_t x)
    {
        // Convert to (0,1) using top 53 bits.
        const std::uint64_t mant = (x >> 11) | 1ULL; // ensure non-zero
        return static_cast<double>(mant) / static_cast<double>(1ULL << 53);
    }

    class PholdLP final : public warpsim::ILogicalProcess
    {
    public:
        PholdLP(warpsim::LPId id, Params params)
            : m_id(id), m_params(params)
        {
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            for (std::uint32_t k = 0; k < m_params.initEventsPerLp; ++k)
            {
                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{0, 0};
                ev.src = m_id;
                ev.dst = m_id;
                sink.send(std::move(ev));
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            // Count total work (includes replays), useful for benchmarking.
            ++m_processed;

            if (ev.ts.time >= m_params.endTime)
            {
                return;
            }

            const std::uint64_t h0 = splitmix64(m_params.seed ^ ev.uid);
            const std::uint64_t h1 = splitmix64(h0);
            const std::uint64_t h2 = splitmix64(h1);

            // Destination.
            warpsim::LPId dst = m_id;
            const double uRemote = u01_from_u64(h0);
            if (uRemote < m_params.remoteFraction)
            {
                dst = static_cast<warpsim::LPId>(h1 % m_params.lps);
                if (m_params.lps > 1 && dst == m_id)
                {
                    dst = static_cast<warpsim::LPId>((dst + 1) % m_params.lps);
                }
            }

            // Exponential timestamp increment with mean = meanLookahead.
            const double u = u01_from_u64(h2);
            const std::uint64_t delta = 1 + static_cast<std::uint64_t>(std::floor(-m_params.meanLookahead * std::log(u)));

            const std::uint64_t nextTime = ev.ts.time + delta;
            if (nextTime > m_params.endTime)
            {
                return;
            }

            warpsim::Event next;
            next.ts = warpsim::TimeStamp{nextTime, 0};
            next.src = m_id;
            next.dst = dst;
            ctx.send(std::move(next));
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

        std::uint64_t processed() const { return m_processed; }

    private:
        warpsim::LPId m_id = 0;
        Params m_params;
        std::uint64_t m_processed = 0;
    };
}

int main(int argc, char **argv)
{
    int rc = MPI_Init(&argc, &argv);
    if (rc != MPI_SUCCESS)
    {
        return 2;
    }

    int rank = -1;
    int size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    const Params p = parse_args(argc, argv, rank);

    auto transport = std::make_shared<warpsim::MpiTransport>(MPI_COMM_WORLD, /*tag=*/0);

    warpsim::SimulationConfig cfg;
    cfg.rank = static_cast<warpsim::RankId>(rank);
    // PDES-style: don't run global collectives every loop; do them periodically.
    // This reduces MPI_Allreduce overhead while keeping correctness.
    cfg.collectivePeriodIters = 256;
    cfg.lpToRank = [size](warpsim::LPId lp) -> warpsim::RankId
    {
        return static_cast<warpsim::RankId>(static_cast<int>(lp % static_cast<warpsim::LPId>(size)));
    };
    cfg.gvtReduceMin = [](warpsim::TimeStamp local) -> warpsim::TimeStamp
    { return warpsim::mpi_allreduce_min_timestamp(MPI_COMM_WORLD, local); };
    cfg.gvtReduceSum = [](std::int64_t local) -> std::int64_t
    { return warpsim::mpi_allreduce_sum_i64(MPI_COMM_WORLD, local); };
    cfg.anyRankHasWork = [](bool localHasWork) -> bool
    { return warpsim::mpi_allreduce_any_work(MPI_COMM_WORLD, localHasWork); };
    cfg.gvtMode = warpsim::SimulationConfig::GvtMode::ColoredCounters;

    warpsim::Simulation sim(cfg, transport);

    for (warpsim::LPId id = 0; id < p.lps; ++id)
    {
        if (static_cast<int>(id % static_cast<warpsim::LPId>(size)) != rank)
        {
            continue;
        }
        auto lp = std::make_unique<PholdLP>(id, p);
        sim.add_lp(std::move(lp));
    }

    sim.run();

    const auto st = sim.stats();

    if (rank == 0)
    {
        std::cout << "PHOLD done: lps=" << p.lps
                  << " end=" << p.endTime
                  << " remote=" << p.remoteFraction
                  << " lookahead=" << p.meanLookahead
                  << " ranks=" << size
                  << "\n";
    }

    std::cout << "rank " << rank << ": gvt=" << st.gvt.time << "." << st.gvt.sequence
              << " pending=" << st.pending
              << " inflight=" << st.inflight
              << " cancelled=" << st.cancelled
              << " processed_kept=" << st.processed
              << " sentlog_kept=" << st.sentLog
              << " totalProcessed=" << st.totalProcessed
              << " rollbacks=" << st.rollbacksTotal
              << " undone=" << st.rollbackUndoneEvents
              << " sent=" << st.sentEvents
              << " sentAnti=" << st.sentAnti
              << " recv=" << st.receivedEvents
              << " recvAnti=" << st.receivedAnti
              << " antiPending=" << st.antiCancelledPending
              << " antiProcessed=" << st.antiCancelledProcessed
              << "\n";

    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();
    return 0;
}
