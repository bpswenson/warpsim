/*
Purpose: Ensures fossil collection runs and advances safely.

What this tests: As logical time progresses and GVT advances, the simulator can discard
old history without breaking execution (basic single-process fossil-collection sanity).
*/

#include "simulation.hpp"

#include <cassert>
#include <cstdint>
#include <memory>

namespace
{
    class NoopLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit NoopLP(warpsim::LPId id, std::uint64_t count) : m_id(id), m_count(count) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Schedule a bunch of strictly-increasing timestamps to advance VT and thus GVT.
            for (std::uint64_t t = 1; t <= m_count; ++t)
            {
                warpsim::Event ev;
                ev.ts = warpsim::TimeStamp{t, 0};
                ev.src = m_id;
                ev.dst = m_id;
                sink.send(ev);
            }
        }

        void on_event(const warpsim::Event &, warpsim::IEventContext &) override {}

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        std::uint64_t m_count = 0;
    };
}

int main()
{
    auto transport = std::make_shared<warpsim::InProcTransport>();
    warpsim::Simulation sim(warpsim::SimulationConfig{.rank = 0}, transport);

    // Use two LPs so GVT is the min VT across them.
    // Both advance together to drive GVT forward.
    sim.add_lp(std::make_unique<NoopLP>(1, 200));
    sim.add_lp(std::make_unique<NoopLP>(2, 200));

    sim.run();

    const auto st = sim.stats();

    // After completion, there are no inflight/pending events.
    assert(st.pending == 0);
    assert(st.inflight == 0);

    // Fossil collection should have pruned almost all processed history.
    // We keep a loose bound to avoid over-constraining implementation details.
    assert(st.processed <= 8);

    return 0;
}
