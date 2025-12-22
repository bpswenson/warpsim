/*
Purpose: Validates snapshot restore when rolling back to the oldest relevant point.

What this tests: Under rollback/anti pressure, the simulator can restore state from the
correct snapshot and produce the right committed outputs after re-execution.
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
    constexpr std::uint32_t KindSet = 10;
    constexpr std::uint32_t KindRead = 11;
    constexpr std::uint32_t KindNoop = 12;
    constexpr std::uint32_t KindCommitted = 200;

    warpsim::ByteBuffer bytes_from_u64(std::uint64_t v)
    {
        std::string s = std::to_string(v);
        warpsim::ByteBuffer out;
        out.resize(s.size());
        std::memcpy(out.data(), s.data(), s.size());
        return out;
    }

    std::uint64_t u64_from_bytes(std::span<const std::byte> b)
    {
        const std::string s(reinterpret_cast<const char *>(b.data()), b.size());
        return static_cast<std::uint64_t>(std::stoull(s));
    }

    struct Output
    {
        warpsim::TimeStamp ts;
        warpsim::Payload payload;
    };

    class StateLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit StateLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindSet)
            {
                // Force an LP snapshot so rollback must restore state.
                ctx.request_write(0);
                m_state = u64_from_bytes(ev.payload.bytes);
                return;
            }

            if (ev.payload.kind == KindRead)
            {
                warpsim::Payload out;
                out.kind = KindCommitted;
                out.bytes = bytes_from_u64(m_state);
                ctx.emit_committed(ev.ts, std::move(out));
                return;
            }

            (void)ctx;
        }

        warpsim::ByteBuffer save_state() const override
        {
            warpsim::ByteBuffer b;
            b.resize(sizeof(m_state));
            std::memcpy(b.data(), &m_state, sizeof(m_state));
            return b;
        }

        void load_state(std::span<const std::byte> bytes) override
        {
            if (bytes.size() != sizeof(m_state))
            {
                // Initial snapshot may be empty if never requested; treat empty as initial state.
                if (bytes.empty())
                {
                    m_state = 0;
                    return;
                }
                throw std::runtime_error("StateLP: unexpected state size");
            }
            std::memcpy(&m_state, bytes.data(), sizeof(m_state));
        }

    private:
        warpsim::LPId m_id = 0;
        std::uint64_t m_state = 0;
    };
}

int main()
{
    // This test reproduces a subtle rollback edge case:
    // - We first process an anti-message at t=8 (no snapshot recorded for that processed record).
    // - Then we process a state-mutating event at t=10 (snapshot recorded).
    // - Then we inject a straggler at t=7, forcing rollback that undoes BOTH records.
    // The oldest undone record is the anti-message. Rollback must still restore LP state
    // using the oldest undone record that actually has a snapshot.

    std::vector<Output> outputs;

    auto transport = std::make_shared<warpsim::InProcTransport>();

    warpsim::SimulationConfig cfg;
    cfg.rank = 0;
    cfg.logLevel = warpsim::LogLevel::Off;
    cfg.committedSink = [&](warpsim::RankId, warpsim::LPId, warpsim::TimeStamp ts, const warpsim::Payload &payload)
    {
        outputs.push_back(Output{ts, payload});
    };

    warpsim::Simulation sim(cfg, transport);
    sim.add_lp(std::make_unique<StateLP>(1));

    // 1) Process an anti-message record at t=8.
    {
        warpsim::Event anti;
        anti.ts = warpsim::TimeStamp{8, 1};
        anti.src = 1;
        anti.dst = 1;
        anti.uid = 0x1111ULL;
        anti.isAnti = true;
        anti.payload.kind = KindNoop;
        sim.send(std::move(anti));
    }

    // 2) Process a state mutation at t=10, taking a snapshot.
    {
        warpsim::Event e;
        e.ts = warpsim::TimeStamp{10, 1};
        e.src = 1;
        e.dst = 1;
        e.payload.kind = KindSet;
        e.payload.bytes = bytes_from_u64(123);
        sim.send(std::move(e));
    }

    bool progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);
    progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // 3) Inject a straggler at t=7 that will force rollback (vt is already >=10).
    {
        warpsim::Event s;
        s.ts = warpsim::TimeStamp{7, 1};
        s.src = 1;
        s.dst = 1;
        s.payload.kind = KindRead;
        sim.send(std::move(s));
    }

    // Run until the straggler is processed.
    progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // Advance time so the committed output from t=7 becomes GVT-safe and is flushed.
    {
        warpsim::Event later;
        later.ts = warpsim::TimeStamp{50, 1};
        later.src = 1;
        later.dst = 1;
        later.payload.kind = KindNoop;
        sim.send(std::move(later));
    }

    progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    // Drain a few more steps to ensure fossil collection has a chance to flush.
    for (int i = 0; i < 32; ++i)
    {
        sim.run_one(/*doCollectives=*/false);
    }

    // We should see the read at t=7 report the *initial* state (0), not the mutated 123.
    assert(!outputs.empty());
    bool found = false;
    for (const auto &o : outputs)
    {
        if (o.ts.time == 7 && o.payload.kind == KindCommitted)
        {
            const auto v = u64_from_bytes(o.payload.bytes);
            assert(v == 0);
            found = true;
            break;
        }
    }
    assert(found);

    return 0;
}
