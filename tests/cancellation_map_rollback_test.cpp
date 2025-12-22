/*
Purpose: Exercises cancellation bookkeeping during rollback.

What this tests: If a receiver rolls back and cancels previously processed work, the
cancellation map/tombstones correctly suppress re-processing and prevent duplicate outputs.
*/

#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::uint32_t KindWork = 500;
    constexpr std::uint32_t KindStraggler = 501;
    constexpr std::uint32_t KindNoop = 502;
    constexpr std::uint32_t KindCommitted = 900;

    warpsim::ByteBuffer bytes_from_string(std::string_view s)
    {
        warpsim::ByteBuffer out;
        out.resize(s.size());
        std::memcpy(out.data(), s.data(), s.size());
        return out;
    }

    std::string string_from_bytes(std::span<const std::byte> b)
    {
        return std::string(reinterpret_cast<const char *>(b.data()), b.size());
    }

    struct Output
    {
        warpsim::TimeStamp ts;
        warpsim::LPId lp;
        warpsim::Payload payload;
    };

    class WorkerLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit WorkerLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindWork)
            {
                warpsim::Payload out;
                out.kind = KindCommitted;
                out.bytes = bytes_from_string("did_work");
                ctx.emit_committed(ev.ts, std::move(out));
                return;
            }

            (void)ctx;
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte> bytes) override
        {
            if (!bytes.empty())
            {
                throw std::runtime_error("WorkerLP: unexpected state bytes");
            }
        }

    private:
        warpsim::LPId m_id = 0;
    };
}

int main()
{
    // Regression test for anti-message rollback bookkeeping:
    // - Process an anti-message that cancels a pending UID.
    // - Force a rollback to before that anti-message via a straggler.
    // - Verify that the cancel-map entry is undone so the cancelled event can execute.

    std::vector<Output> outputs;

    auto transport = std::make_shared<warpsim::InProcTransport>();

    warpsim::SimulationConfig cfg;
    cfg.rank = 0;
    cfg.logLevel = warpsim::LogLevel::Off;
    cfg.committedSink = [&](warpsim::RankId, warpsim::LPId lp, warpsim::TimeStamp ts, const warpsim::Payload &payload)
    {
        outputs.push_back(Output{ts, lp, payload});
    };

    warpsim::Simulation sim(cfg, transport);
    sim.add_lp(std::make_unique<WorkerLP>(1));

    constexpr warpsim::EventUid U = 0x12345678ULL;

    // Event that we will (temporarily) cancel.
    {
        warpsim::Event e;
        e.ts = warpsim::TimeStamp{10, 1};
        e.src = 1;
        e.dst = 1;
        e.uid = U;
        e.payload.kind = KindWork;
        sim.send(std::move(e));
    }

    // Anti-message that cancels the pending UID.
    {
        warpsim::Event anti;
        anti.ts = warpsim::TimeStamp{6, 1};
        anti.src = 1;
        anti.dst = 1;
        anti.uid = U;
        anti.isAnti = true;
        anti.payload.kind = KindNoop;
        sim.send(std::move(anti));
    }

    // Process the anti-message first (t=6) so the cancel map is populated.
    bool progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // Inject a straggler at t=5 to force rollback that undoes the anti-message at t=6.
    {
        warpsim::Event s;
        s.ts = warpsim::TimeStamp{5, 1};
        s.src = 1;
        s.dst = 1;
        s.payload.kind = KindStraggler;
        sim.send(std::move(s));
    }

    // This run triggers rollback (re-enqueues the straggler) but may not process it yet.
    progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // Now process remaining work, including the event at t=10.
    for (int i = 0; i < 64; ++i)
    {
        sim.run_one(/*doCollectives=*/false);
    }

    // Advance time so committed output from t=10 is GVT-safe and flushed.
    {
        warpsim::Event later;
        later.ts = warpsim::TimeStamp{100, 1};
        later.src = 1;
        later.dst = 1;
        later.payload.kind = KindNoop;
        sim.send(std::move(later));
    }
    progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    for (int i = 0; i < 64; ++i)
    {
        sim.run_one(/*doCollectives=*/false);
    }

    // We expect the cancelled event to have executed after rollback removed the tombstone.
    bool found = false;
    for (const auto &o : outputs)
    {
        if (o.lp == 1 && o.ts.time == 10 && o.payload.kind == KindCommitted)
        {
            assert(string_from_bytes(o.payload.bytes) == "did_work");
            found = true;
        }
    }
    assert(found);

    return 0;
}
