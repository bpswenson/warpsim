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
    struct RecvState
    {
        warpsim::EventUid first = 0;
        warpsim::EventUid second = 0;
        std::uint32_t count = 0;
    };

    class ReceiverLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit ReceiverLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            // Ensure rollback safety even if this test ever gets stragglers.
            ctx.request_write(kStateEntity);

            if (m_state.count == 0)
            {
                m_state.first = ev.uid;
            }
            else if (m_state.count == 1)
            {
                m_state.second = ev.uid;
            }
            ++m_state.count;
        }

        warpsim::ByteBuffer save_state() const override
        {
            return warpsim::bytes_from_trivially_copyable(m_state);
        }

        void load_state(std::span<const std::byte> bytes) override
        {
            if (bytes.size() != sizeof(RecvState))
            {
                throw std::runtime_error("ReceiverLP: bad state");
            }
            std::memcpy(&m_state, bytes.data(), sizeof(RecvState));
        }

        RecvState state() const { return m_state; }

    private:
        static constexpr warpsim::EntityId kStateEntity = 0x51554944ULL; // "QUID"

        warpsim::LPId m_id = 0;
        RecvState m_state{};
    };

    class SenderLP final : public warpsim::ILogicalProcess
    {
    public:
        SenderLP(warpsim::LPId id, warpsim::LPId dst) : m_id(id), m_dst(dst) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Two events at the exact same logical time.
            // Their UIDs must be derived from LPId (not rank) and be stable.
            warpsim::Event a;
            a.ts = warpsim::TimeStamp{1, 0};
            a.src = m_id;
            a.dst = m_dst;
            a.payload.kind = 1;
            sink.send(std::move(a));

            warpsim::Event b;
            b.ts = warpsim::TimeStamp{1, 0};
            b.src = m_id;
            b.dst = m_dst;
            b.payload.kind = 2;
            sink.send(std::move(b));
        }

        void on_event(const warpsim::Event &, warpsim::IEventContext &) override {}

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        warpsim::LPId m_dst = 0;
    };

    static RecvState run_once(warpsim::RankId rank)
    {
        auto transport = std::make_shared<warpsim::InProcTransport>();

        warpsim::SimulationConfig cfg;
        cfg.rank = rank;

        warpsim::Simulation sim(cfg, transport);

        auto receiver = std::make_unique<ReceiverLP>(2);
        auto *receiverP = receiver.get();

        sim.add_lp(std::make_unique<SenderLP>(1, /*dst=*/2));
        sim.add_lp(std::move(receiver));

        sim.run();
        return receiverP->state();
    }
}

int main()
{
    const auto s0 = run_once(/*rank=*/0);
    const auto s1 = run_once(/*rank=*/7);

    // UIDs are [srcLPId:32][counter:32]. Sender is LP 1.
    const warpsim::EventUid expectedFirst = (static_cast<std::uint64_t>(1) << 32) | 1ULL;
    const warpsim::EventUid expectedSecond = (static_cast<std::uint64_t>(1) << 32) | 2ULL;

    assert(s0.count == 2);
    assert(s0.first == expectedFirst);
    assert(s0.second == expectedSecond);

    assert(s1.count == 2);
    assert(s1.first == expectedFirst);
    assert(s1.second == expectedSecond);

    return 0;
}
