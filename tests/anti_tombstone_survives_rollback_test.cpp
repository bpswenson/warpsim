/*
Purpose: Ensures cancellations are not lost across rollback.

What this tests: Once an event has been cancelled (via anti), later rollbacks must not
"resurrect" it; cancelled work stays cancelled and committed side effects remain consistent.
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
    constexpr std::uint32_t KindWork = 700;
    constexpr std::uint32_t KindStraggler = 701;
    constexpr std::uint32_t KindNoop = 702;
    constexpr std::uint32_t KindCommitted = 703;

    warpsim::ByteBuffer bytes_from_string(std::string_view s)
    {
        warpsim::ByteBuffer out;
        out.resize(s.size());
        std::memcpy(out.data(), s.data(), s.size());
        return out;
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
    // Anti-message tombstones must survive rollbacks triggered by stragglers:
    // if an anti-message is a real input at t=6 that cancels UID U, then after a rollback to t=5
    // the anti will be replayed and UID U must still be cancelled.

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

    constexpr warpsim::EventUid U = 0xABCDEF01ULL;

    // Event that should be cancelled by the anti-message.
    {
        warpsim::Event e;
        e.ts = warpsim::TimeStamp{10, 1};
        e.src = 1;
        e.dst = 1;
        e.uid = U;
        e.payload.kind = KindWork;
        sim.send(std::move(e));
    }

    // Real anti-message input at t=6.
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

    // Process the anti first.
    bool progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // Force a rollback to before t=6.
    {
        warpsim::Event s;
        s.ts = warpsim::TimeStamp{5, 1};
        s.src = 1;
        s.dst = 1;
        s.payload.kind = KindStraggler;
        sim.send(std::move(s));
    }

    progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // Advance time so any (incorrectly) committed output would flush.
    {
        warpsim::Event later;
        later.ts = warpsim::TimeStamp{200, 1};
        later.src = 1;
        later.dst = 1;
        later.payload.kind = KindNoop;
        sim.send(std::move(later));
    }

    for (int i = 0; i < 256; ++i)
    {
        sim.run_one(/*doCollectives=*/false);
    }

    // The cancelled event must never commit output.
    for (const auto &o : outputs)
    {
        if (o.lp == 1 && o.ts.time == 10 && o.payload.kind == KindCommitted)
        {
            assert(false && "cancelled event committed output unexpectedly");
        }
    }

    return 0;
}
