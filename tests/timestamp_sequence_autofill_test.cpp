#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
    constexpr std::uint32_t KindNoop = 101;

    class NoopLP final : public warpsim::ILogicalProcess
    {
    public:
        NoopLP(warpsim::LPId id, std::vector<std::uint64_t> &seenSeq) : m_id(id), m_seenSeq(seenSeq) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &) override
        {
            if (ev.payload.kind == KindNoop)
            {
                m_seenSeq.push_back(ev.ts.sequence);
            }
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte> bytes) override
        {
            if (!bytes.empty())
            {
                throw std::runtime_error("NoopLP: unexpected state bytes");
            }
        }

    private:
        warpsim::LPId m_id = 0;
        std::vector<std::uint64_t> &m_seenSeq;
    };
}

int main()
{
    // Verifies a core kernel behavior: if caller sends an event with ts.sequence==0,
    // Simulation::send auto-assigns a deterministic per-LP increasing sequence.

    auto transport = std::make_shared<warpsim::InProcTransport>();

    warpsim::SimulationConfig cfg;
    cfg.rank = 0;
    cfg.logLevel = warpsim::LogLevel::Off;

    warpsim::Simulation sim(cfg, transport);
    std::vector<std::uint64_t> seenSeq;
    sim.add_lp(std::make_unique<NoopLP>(1, seenSeq));

    warpsim::Event a;
    a.ts = warpsim::TimeStamp{10, 0};
    a.src = 1;
    a.dst = 1;
    a.payload.kind = KindNoop;
    sim.send(std::move(a));

    warpsim::Event b;
    b.ts = warpsim::TimeStamp{10, 0};
    b.src = 1;
    b.dst = 1;
    b.payload.kind = KindNoop;
    sim.send(std::move(b));

    // Process both.
    bool progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);
    progressed = sim.run_one(/*doCollectives=*/false);
    assert(progressed);

    assert(seenSeq.size() == 2);
    assert(seenSeq[0] == 1);
    assert(seenSeq[1] == 2);

    // With no more work, run_one should return false.
    progressed = sim.run_one(/*doCollectives=*/false);
    assert(!progressed);

    // If this behavior changes, many deterministic examples/tests depend on it.
    return 0;
}
