/*
Purpose: Regression test for sequence determinism across rollback.

What this tests: After rollback, the sender's next auto-assigned sequence counter is
restored, so re-executed sends get the same sequences as the first run.
*/

#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
    constexpr std::uint32_t KindSendLater = 1001;
    constexpr std::uint32_t KindLater = 1002;
    constexpr std::uint32_t KindStraggler = 1003;

    class SeqLP final : public warpsim::ILogicalProcess
    {
    public:
        SeqLP(warpsim::LPId id, std::vector<std::uint64_t> &laterSeqSeen) : m_id(id), m_laterSeqSeen(laterSeqSeen) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindSendLater)
            {
                warpsim::Event later;
                later.ts = warpsim::TimeStamp{20, 0};
                later.src = m_id;
                later.dst = m_id;
                later.payload.kind = KindLater;
                ctx.send(std::move(later));
                return;
            }

            if (ev.payload.kind == KindLater)
            {
                m_laterSeqSeen.push_back(ev.ts.sequence);
                return;
            }

            if (ev.payload.kind == KindStraggler)
            {
                // no-op
                return;
            }
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte> bytes) override
        {
            if (!bytes.empty())
            {
                throw std::runtime_error("SeqLP: unexpected state bytes");
            }
        }

    private:
        warpsim::LPId m_id = 0;
        std::vector<std::uint64_t> &m_laterSeqSeen;
    };
}

int main()
{
    // Regression test: ensure nextSeq is properly restored across rollback so that
    // auto-assigned sequences remain deterministic after re-execution.

    auto transport = std::make_shared<warpsim::InProcTransport>();

    warpsim::SimulationConfig cfg;
    cfg.rank = 0;
    cfg.logLevel = warpsim::LogLevel::Off;

    std::vector<std::uint64_t> laterSeqSeen;

    warpsim::Simulation sim(cfg, transport);
    sim.add_lp(std::make_unique<SeqLP>(1, laterSeqSeen));

    // First, process an event at t=10 that sends a "later" event at t=20 with sequence auto-filled.
    {
        warpsim::Event e;
        e.ts = warpsim::TimeStamp{10, 1};
        e.src = 1;
        e.dst = 1;
        e.payload.kind = KindSendLater;
        sim.send(std::move(e));
    }

    // Run until the first later event (t=20) is processed.
    for (int i = 0; i < 32 && laterSeqSeen.size() < 1; ++i)
    {
        sim.run_one(/*doCollectives=*/false);
    }
    assert(laterSeqSeen.size() == 1);

    // Inject a straggler at t=5 AFTER vt has advanced to 10; this forces rollback.
    {
        warpsim::Event s;
        s.ts = warpsim::TimeStamp{5, 1};
        s.src = 1;
        s.dst = 1;
        s.payload.kind = KindStraggler;
        sim.send(std::move(s));
    }

    // Process the straggler (this triggers rollback).
    bool progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // Rollback should re-enqueue the undone event at t=10, causing the later event at t=20
    // to be re-sent and re-processed. Ensure the auto-assigned sequence is identical.
    for (int i = 0; i < 64 && laterSeqSeen.size() < 2; ++i)
    {
        sim.run_one(/*doCollectives=*/false);
    }

    assert(laterSeqSeen.size() == 2);
    assert(laterSeqSeen[0] == 1);
    assert(laterSeqSeen[1] == 1);
    assert(laterSeqSeen[0] == laterSeqSeen[1]);

    return 0;
}
