/*
Purpose: Migration-related safety test for targeted events.

What this tests:
- A targeted event (target != 0) created with an "unset" dst sentinel must not silently proceed
  when there is no routing mechanism (directoryForEntity or owner map).
- Simulation::send throws with a clear error in this case.
*/

#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

int main()
{
    auto transport = std::make_shared<warpsim::InProcTransport>();

    warpsim::SimulationConfig cfg;
    cfg.rank = 0;

    // Intentionally do NOT set cfg.directoryForEntity.
    // Intentionally do NOT pre-establish any owner map entry.

    warpsim::Simulation sim(cfg, transport);

    warpsim::Event ev;
    ev.ts = warpsim::TimeStamp{1, 0};
    ev.src = 1;
    ev.dst = std::numeric_limits<warpsim::LPId>::max();
    ev.target = 0xABCDEFULL;
    ev.payload.kind = 123;

    bool threw = false;
    try
    {
        sim.send(std::move(ev));
    }
    catch (const std::runtime_error &e)
    {
        threw = true;
        (void)e;
    }

    assert(threw);
    return 0;
}
