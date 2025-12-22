/*
Purpose: Regression test for exactly-once inflight ACK bookkeeping.

What this tests: Under rollback (anti-messages) in an MPI run, the inflight ACK tracking
does not leak or double-count, and the system still reaches a clean termination.
*/

#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>

namespace
{
    constexpr std::uint32_t KindKickoff = 1;
    constexpr std::uint32_t KindWork = 2;
    constexpr std::uint32_t KindStraggler = 3;
    constexpr std::uint32_t KindNoop = 4;
    constexpr std::uint32_t KindCommitted = 200;

    constexpr warpsim::EntityId kStateBase = 0xAC1FF100ULL;

    struct DummyArgs
    {
        std::uint32_t x = 0;
    };

    class MpITestLP final : public warpsim::ILogicalProcess
    {
    public:
        MpITestLP(warpsim::LPId id, int rank, int size)
            : m_id(id), m_rank(rank), m_size(size)
        {
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Test harness: keep global GVT low long enough that the send log cannot be
            // fossil-collected before the synthetic straggler arrives.
            //
            // This test intentionally injects a straggler with a timestamp earlier than the
            // sender's current event time (to force rollback + anti). If GVT were allowed to
            // advance past the kickoff-send point first, the kernel could legally prune the
            // send log and no anti would be emitted. The goal here is to test inflight ACK
            // bookkeeping under rollback, not GVT advancement.
            if (m_rank == 1)
            {
                for (std::uint32_t i = 0; i < 2000; ++i)
                {
                    warpsim::Event hold;
                    hold.ts = warpsim::TimeStamp{0, 0};
                    hold.src = m_id;
                    hold.dst = m_id;
                    hold.payload.kind = KindNoop;
                    sink.send(std::move(hold));
                }
            }

            // Ensure every rank has local work (not just rank0/LP1).
            for (std::uint64_t t = 30; t <= 60; ++t)
            {
                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{t, 0};
                ev.src = m_id;
                ev.dst = m_id;
                ev.payload.kind = KindNoop;
                sink.send(std::move(ev));
            }

            // Only rank0 seeds the kickoff.
            if (m_rank != 0)
            {
                return;
            }

            warpsim::Event kickoff;
            kickoff.ts = warpsim::TimeStamp{10, 1};
            kickoff.src = m_id;
            kickoff.dst = m_id;
            kickoff.payload.kind = KindKickoff;
            sink.send(std::move(kickoff));
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindKickoff)
            {
                // Send an event to LP1 at t=20.
                warpsim::Event out;
                out.ts = warpsim::TimeStamp{20, 0};
                out.src = m_id;
                out.dst = static_cast<warpsim::LPId>(1);
                out.payload.kind = KindWork;
                DummyArgs args{42};
                out.payload.bytes = warpsim::bytes_from_trivially_copyable(args);
                ctx.send(std::move(out));
                return;
            }

            if (ev.payload.kind == KindWork)
            {
                // Only LP1 performs work.
                if (m_id != 1)
                {
                    return;
                }

                // Emit committed output for the work.
                warpsim::Payload out;
                out.kind = KindCommitted;
                out.bytes = warpsim::bytes_from_trivially_copyable(static_cast<std::uint64_t>(1));
                ctx.emit_committed(ev.ts, std::move(out));

                // Now inject a straggler back to LP0 at a timestamp in the past.
                // This will force LP0 to roll back and emit an anti-message for the
                // previously-sent KindWork message.
                warpsim::Event s;
                s.ts = warpsim::TimeStamp{5, 1};
                s.src = m_id;
                s.dst = static_cast<warpsim::LPId>(0);
                s.payload.kind = KindStraggler;
                ctx.send(std::move(s));
                return;
            }

            if (ev.payload.kind == KindNoop)
            {
                ctx.request_write(kStateBase + static_cast<warpsim::EntityId>(m_id));
                ++m_localNoops;
                return;
            }

            // Straggler/noop are intentionally no-ops.
            (void)ctx;
        }

        warpsim::ByteBuffer save_state() const override
        {
            return warpsim::bytes_from_trivially_copyable(m_localNoops);
        }

        void load_state(std::span<const std::byte> state) override
        {
            if (state.size() != sizeof(m_localNoops))
            {
                return;
            }
            std::memcpy(&m_localNoops, state.data(), sizeof(m_localNoops));
        }

    private:
        warpsim::LPId m_id = 0;
        int m_rank = 0;
        int m_size = 1;
        std::uint64_t m_localNoops = 0;
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
    cfg.anyRankHasWork = [](bool localHasWork) -> bool
    { return warpsim::mpi_allreduce_any_work(MPI_COMM_WORLD, localHasWork); };

    // Explicitly test AckInflight mode (transport-level inflight ACKs).
    cfg.gvtMode = warpsim::SimulationConfig::GvtMode::AckInflight;

    cfg.committedSink = [&](warpsim::RankId, warpsim::LPId lp, warpsim::TimeStamp, const warpsim::Payload &payload)
    {
        if (lp == 1 && payload.kind == KindCommitted)
        {
            ++committedCountLocal;
        }
    };

    warpsim::Simulation sim(cfg, transport);

    // One LP per rank; LPId == rank.
    const warpsim::LPId myLp = static_cast<warpsim::LPId>(rank);
    sim.add_lp(std::make_unique<MpITestLP>(myLp, rank, size));

    sim.run();

    const auto st = sim.stats();

    // Anti-gimp: require every rank processed at least one event.
    const std::uint64_t processedLocal = st.totalProcessed;
    std::uint64_t processedMin = 0;
    MPI_Allreduce(&processedLocal, &processedMin, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);

    // Global checks.
    std::uint64_t committedCountGlobal = 0;
    MPI_Allreduce(&committedCountLocal, &committedCountGlobal, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    const std::uint64_t sentAnti = st.sentAnti;
    std::uint64_t sentAntiGlobal = 0;
    MPI_Allreduce(&sentAnti, &sentAntiGlobal, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    const std::uint64_t rollbacksStraggler = st.rollbacksStraggler;
    std::uint64_t rollbacksStragglerGlobal = 0;
    MPI_Allreduce(&rollbacksStraggler, &rollbacksStragglerGlobal, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    const std::uint64_t pendingLocal = static_cast<std::uint64_t>(st.pending);
    const std::uint64_t inflightLocal = static_cast<std::uint64_t>(st.inflight);
    std::uint64_t pendingMax = 0;
    std::uint64_t inflightMax = 0;
    MPI_Allreduce(&pendingLocal, &pendingMax, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&inflightLocal, &inflightMax, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);

    int okLocal = 1;
    // This test is specifically about the inflight ACK bookkeeping in AckInflight mode.
    // We require that a rollback occurred, at least one anti was sent, and the run drained cleanly.
    okLocal &= (sentAntiGlobal >= 1) ? 1 : 0;
    okLocal &= (rollbacksStragglerGlobal >= 1) ? 1 : 0;
    okLocal &= (pendingMax == 0) ? 1 : 0;
    okLocal &= (inflightMax == 0) ? 1 : 0;
    okLocal &= (processedMin > 0) ? 1 : 0;

    int okGlobal = 0;
    MPI_Allreduce(&okLocal, &okGlobal, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);

    if (okGlobal != 1 && rank == 0)
    {
        std::fprintf(stderr,
                     "mpi_ack_inflight_exactly_once failed: processedMin=%llu sentAntiGlobal=%llu rollbacksStragglerGlobal=%llu pendingMax=%llu inflightMax=%llu committedCountGlobal=%llu\n",
                     static_cast<unsigned long long>(processedMin),
                     static_cast<unsigned long long>(sentAntiGlobal),
                     static_cast<unsigned long long>(rollbacksStragglerGlobal),
                     static_cast<unsigned long long>(pendingMax),
                     static_cast<unsigned long long>(inflightMax),
                     static_cast<unsigned long long>(committedCountGlobal));
    }

    MPI_Finalize();
    return (okGlobal == 1) ? 0 : 2;
}
