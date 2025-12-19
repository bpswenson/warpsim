#include "mpi_transport.hpp"
#include "mpi_collectives.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

namespace
{
    static constexpr std::uint32_t kPingKind = 1;

    struct PingArgs
    {
        std::uint32_t remaining = 0;
    };

    class PingLP final : public warpsim::ILogicalProcess
    {
    public:
        PingLP(warpsim::LPId id, int rank, int size, std::uint32_t startRemaining)
            : m_id(id), m_rank(rank), m_size(size), m_startRemaining(startRemaining)
        {
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Only rank0 seeds work; rank1 starts idle.
            if (m_rank != 0)
            {
                return;
            }

            const warpsim::LPId dst = static_cast<warpsim::LPId>(1);

            warpsim::Event ev;
            ev.ts = warpsim::TimeStamp{0, 0};
            ev.src = m_id;
            ev.dst = dst;
            ev.target = 0;
            PingArgs args{m_startRemaining};
            ev.payload.kind = kPingKind;
            ev.payload.bytes = warpsim::bytes_from_trivially_copyable(args);
            sink.send(std::move(ev));
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind != kPingKind || ev.payload.bytes.size() != sizeof(PingArgs))
            {
                throw std::runtime_error("PingLP: unexpected payload");
            }

            PingArgs args{};
            std::memcpy(&args, ev.payload.bytes.data(), sizeof(PingArgs));
            ++m_processed;

            if (args.remaining == 0)
            {
                return;
            }

            const warpsim::LPId other = static_cast<warpsim::LPId>((m_rank + 1) % m_size);
            PingArgs next{static_cast<std::uint32_t>(args.remaining - 1)};

            warpsim::Event out;
            out.ts = warpsim::TimeStamp{ev.ts.time + 1, 0};
            out.src = m_id;
            out.dst = other;
            out.target = 0;
            out.payload.kind = kPingKind;
            out.payload.bytes = warpsim::bytes_from_trivially_copyable(next);
            ctx.send(std::move(out));
        }

        warpsim::ByteBuffer save_state() const override
        {
            return warpsim::bytes_from_trivially_copyable(m_processed);
        }

        void load_state(std::span<const std::byte> state) override
        {
            if (state.size() != sizeof(m_processed))
            {
                throw std::runtime_error("PingLP: bad state size");
            }
            std::memcpy(&m_processed, state.data(), sizeof(m_processed));
        }

    private:
        warpsim::LPId m_id = 0;
        int m_rank = 0;
        int m_size = 1;
        std::uint32_t m_startRemaining = 0;

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

    // One LP per rank.
    const warpsim::LPId myLp = static_cast<warpsim::LPId>(rank);
    sim.add_lp(std::make_unique<PingLP>(myLp, rank, size, /*startRemaining=*/50));

    sim.run();

    const auto stats = sim.stats();

    // Termination correctness signal: both ranks should have received at least one event.
    const bool sawRemote = stats.receivedEvents > 0;

    // Multi-rank GVT correctness signal: after applying gvtReduceMin, all ranks should
    // observe the global minimum (not their local value).
    const warpsim::TimeStamp globalMin = warpsim::mpi_allreduce_min_timestamp(MPI_COMM_WORLD, stats.gvt);
    const bool gvtMatchesGlobal = (stats.gvt.time == globalMin.time) && (stats.gvt.sequence == globalMin.sequence);

    // Also require a non-trivial advance to avoid passing with all-zeros.
    const bool gvtAdvanced = stats.gvt.time > 0;

    int localOk = (sawRemote && gvtMatchesGlobal && gvtAdvanced) ? 1 : 0;
    int globalOk = 0;
    MPI_Allreduce(&localOk, &globalOk, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);

    MPI_Finalize();
    return (globalOk == 1) ? 0 : 2;
}
