#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cassert>
#include <cstdint>
#include <memory>

namespace
{
    class NoopLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit NoopLP(warpsim::LPId id, std::uint64_t count) : m_id(id), m_count(count) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Schedule a bunch of strictly-increasing timestamps to advance VT.
            for (std::uint64_t t = 1; t <= m_count; ++t)
            {
                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{t, 0};
                ev.src = m_id;
                ev.dst = m_id;
                sink.send(ev);
            }
        }

        void on_event(const warpsim::Event &, warpsim::IEventContext &) override {}

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        std::uint64_t m_count = 0;
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

    warpsim::Simulation sim(cfg, transport);

    // One LP per rank. Use LPId==rank so lpToRank(lp % size) routes locally.
    const warpsim::LPId myLp = static_cast<warpsim::LPId>(rank);
    sim.add_lp(std::make_unique<NoopLP>(myLp, 200));

    sim.run();

    const auto st = sim.stats();

    // After completion there are no pending/inflight messages.
    assert(st.pending == 0);
    assert(st.inflight == 0);

    // Global GVT should advance through the scheduled timestamps.
    assert(st.gvt.time >= 200);

    // Fossil collection should have pruned almost all processed history.
    // Use a loose bound to avoid over-constraining implementation details.
    assert(st.processed <= 8);

    MPI_Finalize();
    return 0;
}
