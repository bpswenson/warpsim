#include "simulation.hpp"

#include <cassert>
#include <cstdint>
#include <memory>

namespace
{
    struct LP final : public warpsim::ILogicalProcess
    {
        explicit LP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &) override {}

        void on_event(const warpsim::Event &, warpsim::IEventContext &) override
        {
            // If the ping isn't cancelled, we would get here.
            ++m_counter;
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

        std::uint64_t counter() const { return m_counter; }

    private:
        warpsim::LPId m_id = 0;
        std::uint64_t m_counter = 0;
    };
}

int main()
{
    auto transport = std::make_shared<warpsim::InProcTransport>();
    warpsim::Simulation sim(warpsim::SimulationConfig{.rank = 0}, transport);

    auto lp = std::make_unique<LP>(1);
    auto *lpPtr = lp.get();
    sim.add_lp(std::move(lp));

    // Schedule a normal event and its anti-message such that the anti is processed first.
    const warpsim::EventUid uid = (static_cast<warpsim::EventUid>(2) << 32) ^ 1234ULL;

    warpsim::Event anti;
    anti.ts = warpsim::TimeStamp{5, 0};
    anti.src = 2;
    anti.dst = 1;
    anti.uid = uid;
    anti.isAnti = true;
    sim.schedule(anti);

    warpsim::Event ping;
    ping.ts = warpsim::TimeStamp{10, 0};
    ping.src = 2;
    ping.dst = 1;
    ping.uid = uid;
    ping.isAnti = false;
    sim.schedule(ping);

    sim.run();

    assert(lpPtr->counter() == 0);
    return 0;
}
