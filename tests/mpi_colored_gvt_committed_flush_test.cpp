#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cstdint>
#include <cstring>
#include <memory>

namespace
{
    constexpr std::uint32_t KindSeedSend = 10;
    constexpr std::uint32_t KindWork = 11;
    constexpr std::uint32_t KindNoop = 12;
    constexpr std::uint32_t KindCommitted = 200;

    class GvtLP final : public warpsim::ILogicalProcess
    {
    public:
        GvtLP(warpsim::LPId id, int rank, int size) : m_id(id), m_rank(rank), m_size(size) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Give every rank local work so VT advances and ColoredCounters can advance GVT.
            for (std::uint64_t t = 1; t <= 80; ++t)
            {
                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{t, 0};
                ev.src = m_id;
                ev.dst = m_id;
                ev.payload.kind = KindNoop;
                sink.send(std::move(ev));
            }

            // Only rank0 triggers a single remote work item.
            if (m_rank == 0)
            {
                warpsim::Event seed;
                seed.ts = warpsim::TimeStamp{10, 1};
                seed.src = m_id;
                seed.dst = m_id;
                seed.payload.kind = KindSeedSend;
                sink.send(std::move(seed));
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindSeedSend)
            {
                // Send to LP1 at t=15.
                warpsim::Event out;
                out.ts = warpsim::TimeStamp{15, 0};
                out.src = m_id;
                out.dst = static_cast<warpsim::LPId>(1);
                out.payload.kind = KindWork;
                ctx.send(std::move(out));
                return;
            }

            if (ev.payload.kind == KindWork)
            {
                if (m_id != 1)
                {
                    return;
                }

                warpsim::Payload out;
                out.kind = KindCommitted;
                out.bytes = warpsim::bytes_from_trivially_copyable(static_cast<std::uint64_t>(123));
                ctx.emit_committed(ev.ts, std::move(out));
                return;
            }

            (void)ctx;
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        int m_rank = 0;
        int m_size = 1;
    };
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);

    int rank = 0;
    int size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2)
    {
        MPI_Finalize();
        return 0;
    }

    auto transport = std::make_shared<warpsim::MpiTransport>(MPI_COMM_WORLD);

    std::uint64_t committedCountLocal = 0;

    warpsim::SimulationConfig cfg;
    cfg.rank = static_cast<warpsim::RankId>(rank);
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

    cfg.committedSink = [&](warpsim::RankId, warpsim::LPId lp, warpsim::TimeStamp ts, const warpsim::Payload &payload)
    {
        if (lp == 1 && payload.kind == KindCommitted)
        {
            // Ensure it is emitted at the right timestamp.
            if (ts.time == 15)
            {
                ++committedCountLocal;
            }
        }
    };

    warpsim::Simulation sim(cfg, transport);

    // One LP per rank (LPId == rank).
    const warpsim::LPId myLp = static_cast<warpsim::LPId>(rank);
    sim.add_lp(std::make_unique<GvtLP>(myLp, rank, size));

    sim.run();

    const auto st = sim.stats();

    std::uint64_t committedCountGlobal = 0;
    MPI_Allreduce(&committedCountLocal, &committedCountGlobal, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    // GVT should advance through local work and allow flush.
    const std::uint64_t gvtTime = st.gvt.time;
    std::uint64_t gvtMin = 0;
    MPI_Allreduce(&gvtTime, &gvtMin, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);

    int okLocal = 1;
    okLocal &= (committedCountGlobal == 1) ? 1 : 0;
    okLocal &= (gvtMin >= 80) ? 1 : 0;

    int okGlobal = 0;
    MPI_Allreduce(&okLocal, &okGlobal, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);

    MPI_Finalize();
    return (okGlobal == 1) ? 0 : 2;
}
