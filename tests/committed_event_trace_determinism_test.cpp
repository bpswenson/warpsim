/*
Purpose: Regression test for determinism of the committed-event trace.

What this tests: Even with rollback/anti activity, the sequence of events that become
"committed" (safe past GVT) is deterministic and repeatable.
*/

#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
    constexpr std::uint32_t KindKickoff = 1;
    constexpr std::uint32_t KindWork = 2;
    constexpr std::uint32_t KindStraggler = 3;
    constexpr std::uint32_t KindNoop = 4;
    constexpr std::uint32_t KindCommitted = 200;

    struct TraceItem
    {
        warpsim::LPId lp = 0;
        warpsim::TimeStamp ts{0, 0};
        std::uint32_t kind = 0;
        bool isAnti = false;
    };

    struct CommittedItem
    {
        warpsim::LPId lp = 0;
        warpsim::TimeStamp ts{0, 0};
        std::uint32_t kind = 0;
        warpsim::ByteBuffer bytes;
    };

    class TraceLP final : public warpsim::ILogicalProcess
    {
    public:
        struct Shared
        {
            bool stragglerSent = false;
        };

        TraceLP(warpsim::LPId id, Shared &shared) : m_id(id), m_shared(&shared) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            if (m_id != 0)
            {
                return;
            }

            // Seed initial work and additional noops to drive GVT forward.
            warpsim::Event kickoff;
            kickoff.ts = warpsim::TimeStamp{10, 1};
            kickoff.src = m_id;
            kickoff.dst = m_id;
            kickoff.payload.kind = KindKickoff;
            sink.send(std::move(kickoff));

            for (std::uint64_t t = 30; t <= 80; ++t)
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
            if (ev.payload.kind == KindKickoff)
            {
                // Send a work event to LP1 at t=20.
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

                // Record a rollback-safe side-effect. The kernel must deliver this exactly once
                // once the work event becomes irrevocable.
                {
                    warpsim::Payload p;
                    p.kind = KindCommitted;
                    p.bytes = warpsim::bytes_from_trivially_copyable(static_cast<std::uint64_t>(123));
                    ctx.emit_committed(ev.ts, std::move(p));
                }

                // Trigger exactly one straggler per run to force a rollback/anti,
                // but avoid an infinite rollback loop in single-rank/in-proc runs.
                if (m_shared && !m_shared->stragglerSent)
                {
                    m_shared->stragglerSent = true;
                    warpsim::Event s;
                    s.ts = warpsim::TimeStamp{5, 1};
                    s.src = m_id;
                    s.dst = 0;
                    s.payload.kind = KindStraggler;
                    ctx.send(std::move(s));
                }
                return;
            }

            (void)ctx;
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        Shared *m_shared = nullptr;
    };

    struct RunResult
    {
        std::vector<TraceItem> events;
        std::vector<CommittedItem> committed;
    };

    RunResult run_and_trace(bool enableChecks)
    {
        auto transport = std::make_shared<warpsim::InProcTransport>();

        RunResult out;

        TraceLP::Shared shared;

        warpsim::SimulationConfig cfg;
        cfg.collectivePeriodIters = 1;
        cfg.enableInvariantChecks = enableChecks;
        cfg.committedEventSink = [&](warpsim::RankId, warpsim::LPId lp, const warpsim::Event &ev)
        {
            out.events.push_back(TraceItem{lp, ev.ts, ev.payload.kind, ev.isAnti});
        };
        cfg.committedSink = [&](warpsim::RankId, warpsim::LPId lp, warpsim::TimeStamp ts, const warpsim::Payload &payload)
        {
            out.committed.push_back(CommittedItem{lp, ts, payload.kind, payload.bytes});
        };

        warpsim::Simulation sim(cfg, transport);
        sim.add_lp(std::make_unique<TraceLP>(0, shared));
        sim.add_lp(std::make_unique<TraceLP>(1, shared));

        sim.run();

        return out;
    }
}

int main()
{
    const auto r1 = run_and_trace(/*enableChecks=*/true);
    const auto r2 = run_and_trace(/*enableChecks=*/true);

    const auto &t1 = r1.events;
    const auto &t2 = r2.events;

    // Determinism oracle: committed-event trace should be identical across identical runs.
    assert(t1.size() == t2.size());
    for (std::size_t i = 0; i < t1.size(); ++i)
    {
        assert(t1[i].lp == t2[i].lp);
        assert(t1[i].ts.time == t2[i].ts.time);
        assert(t1[i].ts.sequence == t2[i].ts.sequence);
        assert(t1[i].kind == t2[i].kind);
        assert(t1[i].isAnti == t2[i].isAnti);
    }

    // Also require that the non-anti work event ultimately commits exactly once on LP1.
    std::uint64_t workCommits = 0;
    std::uint64_t antiCommits = 0;
    for (const auto &it : t1)
    {
        if (it.isAnti)
        {
            ++antiCommits;
        }
        if (!it.isAnti && it.kind == KindWork)
        {
            ++workCommits;
            assert(it.lp == 1);
        }
    }
    assert(workCommits == 1);
    assert(antiCommits >= 1);

    // Determinism oracle: committed output trace should be identical too.
    assert(r1.committed.size() == r2.committed.size());
    for (std::size_t i = 0; i < r1.committed.size(); ++i)
    {
        assert(r1.committed[i].lp == r2.committed[i].lp);
        assert(r1.committed[i].ts.time == r2.committed[i].ts.time);
        assert(r1.committed[i].ts.sequence == r2.committed[i].ts.sequence);
        assert(r1.committed[i].kind == r2.committed[i].kind);
        assert(r1.committed[i].bytes == r2.committed[i].bytes);
    }

    // Exactly-once committed side effect for the work event.
    std::uint64_t committedCount = 0;
    for (const auto &c : r1.committed)
    {
        if (c.kind == KindCommitted)
        {
            ++committedCount;
            assert(c.lp == 1);
            assert(c.ts.time == 20);
        }
    }
    assert(committedCount == 1);

    return 0;
}
