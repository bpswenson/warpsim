#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>

namespace
{
    constexpr std::uint32_t KindInc = 100;

    struct State
    {
        std::uint64_t counter = 0;
    };

    class CounterLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit CounterLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind != KindInc)
            {
                return;
            }

            // Snapshot before mutation (LP-wide snapshots are fine for this test).
            ctx.request_write(kStateEntity);
            ++m_state.counter;
        }

        warpsim::ByteBuffer save_state() const override
        {
            return warpsim::bytes_from_trivially_copyable(m_state);
        }

        void load_state(std::span<const std::byte> bytes) override
        {
            if (bytes.size() != sizeof(State))
            {
                throw std::runtime_error("CounterLP: bad state");
            }
            std::memcpy(&m_state, bytes.data(), sizeof(State));
        }

        State state() const { return m_state; }

    private:
        static constexpr warpsim::EntityId kStateEntity = 0xC0A7ULL;

        warpsim::LPId m_id = 0;
        State m_state{};
    };
}

int main()
{
    auto transport = std::make_shared<warpsim::InProcTransport>();

    warpsim::SimulationConfig cfg;
    cfg.rank = 0;

    warpsim::Simulation sim(cfg, transport);

    auto lp = std::make_unique<CounterLP>(1);
    auto *lpP = lp.get();
    sim.add_lp(std::move(lp));

    // Enqueue a later event, then step until it's processed.
    {
        warpsim::Event e;
        e.ts = warpsim::TimeStamp{10, 0};
        e.src = 1;
        e.dst = 1;
        e.payload.kind = KindInc;
        sim.send(std::move(e));
    }

    // Process exactly one event (the t=10 one).
    bool progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);
    assert(lpP->state().counter == 1);

    // Now inject a straggler at t=5, which must cause rollback and then replay t=10.
    {
        warpsim::Event e;
        e.ts = warpsim::TimeStamp{5, 0};
        e.src = 1;
        e.dst = 1;
        e.payload.kind = KindInc;
        sim.send(std::move(e));
    }

    // Drive the sim to quiescence using run_one.
    for (int i = 0; i < 1000; ++i)
    {
        if (!sim.run_one(/*doCollectives=*/false))
        {
            break;
        }
    }

    // Both events should have been applied exactly once in final state.
    assert(lpP->state().counter == 2);
    return 0;
}
