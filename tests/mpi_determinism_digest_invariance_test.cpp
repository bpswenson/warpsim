/*
Purpose: Rank-count invariance oracle for determinism.

What this tests: A deterministic scenario (cross-rank messaging + at least one rollback
via a guarded straggler + committed output) produces the same global digest under
different numbers of MPI ranks.
*/

#include "determinism.hpp"
#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace
{
    constexpr std::uint32_t KindKickoff = 1;
    constexpr std::uint32_t KindWork = 2;
    constexpr std::uint32_t KindNoop = 3;
    constexpr std::uint32_t KindCommitted = 200;

    // Heartbeat events ensure every rank runs at least one local, stateful event.
    // They are filtered out of the determinism digest so the expected values remain stable.
    constexpr std::uint32_t KindHeartbeat = 999;
    constexpr warpsim::EntityId kStateBase = 0xD1735100ULL;

    // A deterministic scenario that:
    // - sends cross-rank messages
    // - triggers at least one rollback via a single guarded straggler
    // - emits exactly-once committed output
    class DigestLP final : public warpsim::ILogicalProcess
    {
    public:
        struct Shared
        {
            bool stragglerSent = false;
        };

        DigestLP(warpsim::LPId id, int rank, int size, Shared &shared)
            : m_id(id), m_rank(rank), m_size(size), m_shared(&shared)
        {
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Per-LP local heartbeat (stateful) so all ranks do real work.
            warpsim::Event hb;
            hb.ts = warpsim::TimeStamp{0, 0};
            hb.src = m_id;
            hb.dst = m_id;
            hb.payload.kind = KindHeartbeat;
            sink.send(std::move(hb));

            // Only LP0 seeds the kickoff.
            if (m_id != 0)
            {
                return;
            }

            warpsim::Event kickoff;
            kickoff.ts = warpsim::TimeStamp{10, 1};
            kickoff.src = m_id;
            kickoff.dst = m_id;
            kickoff.payload.kind = KindKickoff;
            sink.send(std::move(kickoff));

            // Extra noops to drive GVT forward and terminate.
            for (std::uint64_t t = 30; t <= 120; ++t)
            {
                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{t, 0};
                ev.src = m_id;
                ev.dst = m_id;
                ev.payload.kind = KindNoop;
                sink.send(std::move(ev));
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindHeartbeat)
            {
                ctx.request_write(kStateBase + static_cast<warpsim::EntityId>(m_id));
                ++m_heartbeats;
                return;
            }

            if (ev.payload.kind == KindKickoff)
            {
                // Send work to a fixed LP (1) at t=20.
                warpsim::Event out;
                out.ts = warpsim::TimeStamp{20, 0};
                out.src = m_id;
                out.dst = 1;
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

                // Emit committed output tied to this work.
                warpsim::Payload p;
                p.kind = KindCommitted;
                p.bytes = warpsim::bytes_from_trivially_copyable(static_cast<std::uint64_t>(123));
                ctx.emit_committed(ev.ts, std::move(p));

                // Inject exactly one straggler per run (out-of-band guard) to force rollback/anti.
                if (m_shared && !m_shared->stragglerSent)
                {
                    m_shared->stragglerSent = true;
                    warpsim::Event s;
                    s.ts = warpsim::TimeStamp{5, 1};
                    s.src = m_id;
                    s.dst = 0;
                    s.payload.kind = KindNoop;
                    ctx.send(std::move(s));
                }
                return;
            }

            (void)ctx;
        }

        warpsim::ByteBuffer save_state() const override
        {
            return warpsim::bytes_from_trivially_copyable(m_heartbeats);
        }

        void load_state(std::span<const std::byte> state) override
        {
            if (state.size() != sizeof(m_heartbeats))
            {
                return;
            }
            std::memcpy(&m_heartbeats, state.data(), sizeof(m_heartbeats));
        }

    private:
        warpsim::LPId m_id = 0;
        int m_rank = 0;
        int m_size = 1;
        Shared *m_shared = nullptr;
        std::uint64_t m_heartbeats = 0;
    };

    // Reduce a DeterminismDigest across ranks.
    warpsim::DeterminismDigest allreduce_digest(MPI_Comm comm, const warpsim::DeterminismDigest &local)
    {
        warpsim::DeterminismDigest out{};

        // SUM components.
        MPI_Allreduce(&local.committedEventSum, &out.committedEventSum, 1, MPI_UINT64_T, MPI_SUM, comm);
        MPI_Allreduce(&local.committedOutputSum, &out.committedOutputSum, 1, MPI_UINT64_T, MPI_SUM, comm);

        // XOR components.
        MPI_Allreduce(&local.committedEventXor, &out.committedEventXor, 1, MPI_UINT64_T, MPI_BXOR, comm);
        MPI_Allreduce(&local.committedOutputXor, &out.committedOutputXor, 1, MPI_UINT64_T, MPI_BXOR, comm);

        return out;
    }
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

    warpsim::DeterminismAccumulator acc;

    warpsim::SimulationConfig cfg;
    cfg.rank = static_cast<warpsim::RankId>(rank);
    cfg.enableInvariantChecks = true;
    cfg.lpToRank = [size](warpsim::LPId lp) -> warpsim::RankId
    {
        return static_cast<warpsim::RankId>(static_cast<int>(lp % static_cast<warpsim::LPId>(size)));
    };

    cfg.gvtReduceMin = [](warpsim::TimeStamp local) -> warpsim::TimeStamp
    { return warpsim::mpi_allreduce_min_timestamp(MPI_COMM_WORLD, local); };
    cfg.anyRankHasWork = [](bool localHasWork) -> bool
    { return warpsim::mpi_allreduce_any_work(MPI_COMM_WORLD, localHasWork); };

    cfg.committedEventSink = [&](warpsim::RankId r, warpsim::LPId lp, const warpsim::Event &ev)
    {
        if (ev.payload.kind == KindHeartbeat)
        {
            return;
        }
        acc.on_committed_event(r, lp, ev);
    };
    cfg.committedSink = [&](warpsim::RankId r, warpsim::LPId lp, warpsim::TimeStamp ts, const warpsim::Payload &p)
    { acc.on_committed_output(r, lp, ts, p); };

    warpsim::Simulation sim(cfg, transport);

    DigestLP::Shared shared;

    // Fixed LP population independent of MPI size.
    constexpr warpsim::LPId kNumLps = 8;
    for (warpsim::LPId lp = 0; lp < kNumLps; ++lp)
    {
        if (cfg.lpToRank(lp) != cfg.rank)
        {
            continue;
        }
        sim.add_lp(std::make_unique<DigestLP>(lp, rank, size, shared));
    }

    sim.run();

    const warpsim::DeterminismDigest global = allreduce_digest(MPI_COMM_WORLD, acc.digest());

    // Hard-coded expected digest is filled in after first run.
    // This should be identical for any rank count (>=2) for this scenario.
    const warpsim::DeterminismDigest expected{
        /*committedEventSum=*/17590672556510665218ULL,
        /*committedEventXor=*/14948406094151098126ULL,
        /*committedOutputSum=*/12534452722119763232ULL,
        /*committedOutputXor=*/12534452722119763232ULL,
    };

    int okLocal = (global == expected) ? 1 : 0;

    if (rank == 0 && okLocal != 1)
    {
        std::fprintf(stderr,
                     "MPI determinism digest mismatch (size=%d):\n  got  evSum=%llu evXor=%llu outSum=%llu outXor=%llu\n  want evSum=%llu evXor=%llu outSum=%llu outXor=%llu\n",
                     size,
                     static_cast<unsigned long long>(global.committedEventSum),
                     static_cast<unsigned long long>(global.committedEventXor),
                     static_cast<unsigned long long>(global.committedOutputSum),
                     static_cast<unsigned long long>(global.committedOutputXor),
                     static_cast<unsigned long long>(expected.committedEventSum),
                     static_cast<unsigned long long>(expected.committedEventXor),
                     static_cast<unsigned long long>(expected.committedOutputSum),
                     static_cast<unsigned long long>(expected.committedOutputXor));
    }

    int okGlobal = 0;
    MPI_Allreduce(&okLocal, &okGlobal, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);

    MPI_Finalize();
    return (okGlobal == 1) ? 0 : 2;
}
