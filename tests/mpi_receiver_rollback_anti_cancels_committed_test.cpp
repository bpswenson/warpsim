/*
Purpose: Ensures receiver rollback cancels committed side effects before they flush.

What this tests: In an MPI run, if a receiver rolls back and sends an anti-message for a
work event, any committed output produced by that work is cancelled and does not flush.
*/

#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cstdint>
#include <cstring>
#include <memory>

namespace
{
    constexpr std::uint32_t KindKickoff = 1;
    constexpr std::uint32_t KindWork = 2;
    constexpr std::uint32_t KindSendAck = 3;
    constexpr std::uint32_t KindAck = 4;
    constexpr std::uint32_t KindDummy = 5;
    constexpr std::uint32_t KindCommitted = 200;

    constexpr std::uint32_t KindHeartbeat = 999;
    constexpr warpsim::EntityId kHeartbeatStateBase = 0xBEEFB001ULL;

    constexpr warpsim::EventUid WorkUid = 0x55555555ULL;

    class ReceiverRollbackLP final : public warpsim::ILogicalProcess
    {
    public:
        ReceiverRollbackLP(warpsim::LPId id, int rank) : m_id(id), m_rank(rank) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Ensure there is always at least one LP with vt=0 until late in the run,
            // so committed output at t=20 cannot be flushed before the anti arrives.
            if (m_id == 2)
            {
                warpsim::Event dummy;
                dummy.ts = warpsim::TimeStamp{100, 1};
                dummy.src = m_id;
                dummy.dst = m_id;
                dummy.payload.kind = KindDummy;
                sink.send(std::move(dummy));
                return;
            }

            // Rank0 seeds the kickoff.
            if (m_rank == 0 && m_id == 0)
            {
                warpsim::Event kickoff;
                kickoff.ts = warpsim::TimeStamp{10, 1};
                kickoff.src = m_id;
                kickoff.dst = m_id;
                kickoff.payload.kind = KindKickoff;
                sink.send(std::move(kickoff));
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindKickoff)
            {
                // Send work to LP1 at t=20 with a fixed UID so a later anti can target it.
                warpsim::Event work;
                work.ts = warpsim::TimeStamp{20, 1};
                work.src = m_id;
                work.dst = 1;
                work.uid = WorkUid;
                work.payload.kind = KindWork;
                ctx.send(std::move(work));
                return;
            }

            if (ev.payload.kind == KindWork)
            {
                // LP1 emits a committed side effect at t=20, then schedules an ack-sender at t=30.
                if (m_id != 1)
                {
                    return;
                }

                warpsim::Payload out;
                out.kind = KindCommitted;
                out.bytes = warpsim::bytes_from_trivially_copyable(static_cast<std::uint64_t>(1));
                ctx.emit_committed(ev.ts, std::move(out));

                warpsim::Event sendAck;
                sendAck.ts = warpsim::TimeStamp{30, 1};
                sendAck.src = m_id;
                sendAck.dst = m_id;
                sendAck.payload.kind = KindSendAck;
                ctx.send(std::move(sendAck));
                return;
            }

            if (ev.payload.kind == KindSendAck)
            {
                // LP1 tells LP0 it has advanced vt beyond t=20.
                if (m_id != 1)
                {
                    return;
                }

                warpsim::Event ack;
                ack.ts = warpsim::TimeStamp{30, 2};
                ack.src = m_id;
                ack.dst = 0;
                ack.payload.kind = KindAck;
                ctx.send(std::move(ack));
                return;
            }

            if (ev.payload.kind == KindAck)
            {
                // LP0 injects a late anti-message for the original work at t=20.
                // This should arrive at LP1 after it has advanced vt to >= 30, forcing receiver rollback.
                if (m_id != 0)
                {
                    return;
                }

                warpsim::Event anti;
                anti.ts = warpsim::TimeStamp{20, 1};
                anti.src = m_id;
                anti.dst = 1;
                anti.uid = WorkUid;
                anti.isAnti = true;
                anti.payload.kind = 0;
                ctx.send(std::move(anti));
                return;
            }

            // Dummy/no-op.
            (void)ctx;
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        int m_rank = 0;
    };

    // A tiny per-rank LP used only to prove that each rank runs at least one stateful event.
    class HeartbeatLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit HeartbeatLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            warpsim::Event hb;
            hb.ts = warpsim::TimeStamp{1, 0};
            hb.src = m_id;
            hb.dst = m_id;
            hb.payload.kind = KindHeartbeat;
            sink.send(std::move(hb));
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind != KindHeartbeat)
            {
                return;
            }
            ctx.request_write(kHeartbeatStateBase + static_cast<warpsim::EntityId>(m_id));
            ++m_count;
        }

        warpsim::ByteBuffer save_state() const override
        {
            return warpsim::bytes_from_trivially_copyable(m_count);
        }

        void load_state(std::span<const std::byte> state) override
        {
            if (state.size() != sizeof(m_count))
            {
                return;
            }
            std::memcpy(&m_count, state.data(), sizeof(m_count));
        }

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

    // Exercise conservative AckInflight mode under MPI.
    cfg.gvtMode = warpsim::SimulationConfig::GvtMode::AckInflight;

    cfg.committedSink = [&](warpsim::RankId, warpsim::LPId lp, warpsim::TimeStamp ts, const warpsim::Payload &payload)
    {
        // The work at t=20 should be cancelled by the late anti, so it must never be emitted.
        if (lp == 1 && payload.kind == KindCommitted && ts.time == 20)
        {
            ++committedCountLocal;
        }
    };

    warpsim::Simulation sim(cfg, transport);

    // Add a per-rank heartbeat LP that maps locally via (lp % size).
    // This keeps the core size=2 layout intact while ensuring size>2 runs are non-gimped.
    const warpsim::LPId heartbeatLp = static_cast<warpsim::LPId>(rank + size * 10);
    sim.add_lp(std::make_unique<HeartbeatLP>(heartbeatLp));

    // LP layout for size=2 (lp%2): rank0 owns LP0 and LP2, rank1 owns LP1.
    if (rank == 0)
    {
        sim.add_lp(std::make_unique<ReceiverRollbackLP>(0, rank));
        sim.add_lp(std::make_unique<ReceiverRollbackLP>(2, rank));
    }
    else
    {
        sim.add_lp(std::make_unique<ReceiverRollbackLP>(1, rank));
    }

    sim.run();

    const auto st = sim.stats();
    const std::uint64_t processedLocal = st.totalProcessed;
    std::uint64_t processedMin = 0;
    MPI_Allreduce(&processedLocal, &processedMin, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);

    std::uint64_t committedCountGlobal = 0;
    MPI_Allreduce(&committedCountLocal, &committedCountGlobal, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    int okLocal = 1;
    okLocal &= (committedCountGlobal == 0) ? 1 : 0;
    okLocal &= (processedMin > 0) ? 1 : 0;
    int okGlobal = 0;
    MPI_Allreduce(&okLocal, &okGlobal, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);

    MPI_Finalize();
    return (okGlobal == 1) ? 0 : 2;
}
