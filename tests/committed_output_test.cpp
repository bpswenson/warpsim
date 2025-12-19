#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace
{
    constexpr std::uint32_t KindEmit = 100;
    constexpr std::uint32_t KindNoop = 101;
    constexpr std::uint32_t KindCommitted = 200;

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

    class EmitterLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit EmitterLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindEmit)
            {
                warpsim::Payload out;
                out.kind = KindCommitted;
                out.bytes = bytes_from_string("side_effect");
                ctx.emit_committed(ev.ts, std::move(out));
            }
        }

        warpsim::ByteBuffer save_state() const override { return {}; }

        void load_state(std::span<const std::byte> bytes) override
        {
            if (!bytes.empty())
            {
                throw std::runtime_error("EmitterLP: unexpected state bytes");
            }
        }

    private:
        warpsim::LPId m_id = 0;
    };
}

int main()
{
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

    auto lp = std::make_unique<EmitterLP>(1);
    sim.add_lp(std::move(lp));

    // Enqueue an event at t=50 that requests a committed side effect.
    {
        warpsim::Event e;
        e.ts = warpsim::TimeStamp{50, 0};
        e.src = 1;
        e.dst = 1;
        e.payload.kind = KindEmit;
        sim.send(std::move(e));
    }

    // Process only that one event. Even after processing, output must not be visible
    // until fossil collection advances GVT beyond t=50.
    bool progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);
    assert(outputs.empty());

    // Advance time/GVT by sending and processing a later event at t=60.
    {
        warpsim::Event e;
        e.ts = warpsim::TimeStamp{60, 0};
        e.src = 1;
        e.dst = 1;
        e.payload.kind = KindNoop;
        sim.send(std::move(e));
    }

    progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    assert(outputs.size() == 1);
    const warpsim::TimeStamp expectedTs{50, 0};
    assert(outputs[0].ts == expectedTs);
    assert(outputs[0].lp == 1);
    assert(outputs[0].payload.kind == KindCommitted);
    assert(string_from_bytes(outputs[0].payload.bytes) == "side_effect");

    // Ensure it doesn't get emitted again.
    for (int i = 0; i < 10; ++i)
    {
        sim.run_one(/*doCollectives=*/false);
    }
    assert(outputs.size() == 1);

    return 0;
}
