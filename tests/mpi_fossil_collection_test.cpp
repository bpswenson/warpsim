/*
Purpose: Exercises fossil collection in an MPI run.

What this tests: With one LP per rank and advancing local virtual time, global GVT
advances via MPI collectives and the simulator finishes with no pending/inflight work.
*/

#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace
{
    constexpr warpsim::EntityId kStateBase = 0xF0551100ULL;

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

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            ctx.request_write(kStateBase + static_cast<warpsim::EntityId>(m_id));
            ++m_processed;
            (void)ev;
        }

        warpsim::ByteBuffer save_state() const override
        {
            return warpsim::bytes_from_trivially_copyable(m_processed);
        }

        void load_state(std::span<const std::byte> state) override
        {
            if (state.size() != sizeof(m_processed))
            {
                return;
            }
            std::memcpy(&m_processed, state.data(), sizeof(m_processed));
        }

    private:
        warpsim::LPId m_id = 0;
        std::uint64_t m_count = 0;
        std::uint64_t m_processed = 0;
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

    // Anti-gimp: every rank should process at least one event.
    const std::uint64_t processedLocal = st.totalProcessed;
    std::uint64_t processedMin = 0;
    MPI_Allreduce(&processedLocal, &processedMin, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);

    std::uint64_t pendingMax = 0;
    std::uint64_t inflightMax = 0;
    std::uint64_t gvtMin = 0;
    std::uint64_t processedMax = 0;

    const std::uint64_t pendingLocal = static_cast<std::uint64_t>(st.pending);
    const std::uint64_t inflightLocal = static_cast<std::uint64_t>(st.inflight);
    const std::uint64_t gvtLocal = static_cast<std::uint64_t>(st.gvt.time);
    const std::uint64_t processedLocalCur = static_cast<std::uint64_t>(st.processed);

    MPI_Allreduce(&pendingLocal, &pendingMax, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&inflightLocal, &inflightMax, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&gvtLocal, &gvtMin, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);
    MPI_Allreduce(&processedLocalCur, &processedMax, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);

    int okLocal = 1;
    okLocal &= (processedMin > 0) ? 1 : 0;
    okLocal &= (pendingMax == 0) ? 1 : 0;
    okLocal &= (inflightMax == 0) ? 1 : 0;
    okLocal &= (gvtMin >= 200) ? 1 : 0;
    okLocal &= (processedMax <= 8) ? 1 : 0;

    int okGlobal = 0;
    MPI_Allreduce(&okLocal, &okGlobal, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);

    if (rank == 0 && okGlobal != 1)
    {
        std::fprintf(stderr,
                     "mpi_fossil_collection_test failed: processedMin=%llu pendingMax=%llu inflightMax=%llu gvtMin=%llu processedMax=%llu\n",
                     static_cast<unsigned long long>(processedMin),
                     static_cast<unsigned long long>(pendingMax),
                     static_cast<unsigned long long>(inflightMax),
                     static_cast<unsigned long long>(gvtMin),
                     static_cast<unsigned long long>(processedMax));
    }

    MPI_Finalize();
    return (okGlobal == 1) ? 0 : 2;
}
