/*
Purpose: Ensures rollback emits anti-messages for previously sent events.

What this tests: The sender's sent-log is complete enough to generate matching anti
messages during rollback, so downstream LPs correctly cancel and do not commit side effects.
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
    constexpr std::uint32_t KindSendTo2 = 600;
    constexpr std::uint32_t KindFrom1 = 601;
    constexpr std::uint32_t KindStraggler = 602;
    constexpr std::uint32_t KindNoop = 603;
    constexpr std::uint32_t KindCommitted = 999;

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

    class SenderLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit SenderLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindSendTo2)
            {
                warpsim::Event out;
                out.ts = warpsim::TimeStamp{20, 0};
                out.src = m_id;
                out.dst = 2;
                out.payload.kind = KindFrom1;
                ctx.send(std::move(out));
                return;
            }

            (void)ctx;
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte> bytes) override
        {
            if (!bytes.empty())
            {
                throw std::runtime_error("SenderLP: unexpected state bytes");
            }
        }

    private:
        warpsim::LPId m_id = 0;
    };

    class ReceiverLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit ReceiverLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindFrom1)
            {
                warpsim::Payload out;
                out.kind = KindCommitted;
                out.bytes = bytes_from_string("received");
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
                throw std::runtime_error("ReceiverLP: unexpected state bytes");
            }
        }

    private:
        warpsim::LPId m_id = 0;
    };
}

int main()
{
    // Regression test for sent-log rollback cancelling:
    // - LP1 processes an event that sends an event to LP2.
    // - Then LP1 receives a straggler that forces rollback.
    // - Rollback should emit an anti-message for the sent event, cancelling it at LP2.
    // - Verify LP2's side effect is not committed.

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
    sim.add_lp(std::make_unique<SenderLP>(1));
    sim.add_lp(std::make_unique<ReceiverLP>(2));

    // Event at t=10 causes LP1 to send to LP2 at t=20.
    {
        warpsim::Event e;
        e.ts = warpsim::TimeStamp{10, 1};
        e.src = 1;
        e.dst = 1;
        e.payload.kind = KindSendTo2;
        sim.send(std::move(e));
    }

    // Process the send.
    bool progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // Force rollback of LP1 to before t=10.
    {
        warpsim::Event s;
        s.ts = warpsim::TimeStamp{5, 1};
        s.src = 1;
        s.dst = 1;
        s.payload.kind = KindStraggler;
        sim.send(std::move(s));
    }

    // This triggers rollback and emits an anti-message for the sent event.
    progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // Advance time well beyond t=20 to allow any committed outputs to flush.
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

    // There should be no committed output from LP2 at t=20.
    for (const auto &o : outputs)
    {
        if (o.lp == 2 && o.ts.time == 20 && o.payload.kind == KindCommitted)
        {
            (void)string_from_bytes(o.payload.bytes);
            assert(false && "LP2 committed output should have been cancelled by anti-message");
        }
    }

    return 0;
}
