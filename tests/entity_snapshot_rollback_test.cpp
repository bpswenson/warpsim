/*
Purpose: Checks that entity-level snapshot/restore is correct during rollback.

What this tests: With intentionally reordered message delivery (stragglers), the engine
must roll back and restore per-entity state (multiple entities) to the right values.
*/

#include "event_dispatcher.hpp"
#include "simulation.hpp"
#include "world.hpp"

#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace
{
    using EventTypeId = std::uint32_t;

    enum : EventTypeId
    {
        High_WriteBoth = 4001,
        Low_WriteA = 4002,
    };

    struct A
    {
        std::uint64_t value = 0;
    };

    struct B
    {
        std::uint64_t value = 0;
    };

    // Transport that drips messages to reorder delivery and induce stragglers.
    class ThrottledInProcTransport final : public warpsim::ITransport
    {
    public:
        void send(warpsim::WireMessage msg) override
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_queue.push_back(std::move(msg));
        }

        std::optional<warpsim::WireMessage> poll() override
        {
            std::lock_guard<std::mutex> lk(m_mu);

            if (m_throttle)
            {
                m_throttle = false;
                return std::nullopt;
            }

            if (m_queue.empty())
            {
                return std::nullopt;
            }

            auto msg = std::move(m_queue.front());
            m_queue.pop_front();
            m_throttle = true;
            return msg;
        }

        bool has_pending() const override
        {
            std::lock_guard<std::mutex> lk(m_mu);
            return !m_queue.empty();
        }

    private:
        mutable std::mutex m_mu;
        std::deque<warpsim::WireMessage> m_queue;
        bool m_throttle = false;
    };

    class LP1 final : public warpsim::ILogicalProcess
    {
    public:
        explicit LP1(warpsim::LPId id, warpsim::LPId peer) : m_id(id), m_peer(peer) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            const warpsim::EntityId remoteA = make_id_(m_peer, 1);

            // High-time message first (will be delivered first) then straggler.
            warpsim::Event high;
            high.ts = warpsim::TimeStamp{10, 0};
            high.src = m_id;
            high.dst = m_peer;
            high.target = remoteA;
            high.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(High_WriteBoth, std::uint32_t{0});
            sink.send(high);

            warpsim::Event low;
            low.ts = warpsim::TimeStamp{5, 0};
            low.src = m_id;
            low.dst = m_peer;
            low.target = remoteA;
            low.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Low_WriteA, std::uint32_t{0});
            sink.send(low);
        }

        void on_event(const warpsim::Event &, warpsim::IEventContext &) override {}

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        static constexpr warpsim::EntityId make_id_(warpsim::LPId owner, std::uint32_t local)
        {
            return (static_cast<warpsim::EntityId>(owner) << 32) | static_cast<warpsim::EntityId>(local);
        }

        warpsim::LPId m_id = 0;
        warpsim::LPId m_peer = 0;
    };

    class LP2 final : public warpsim::ILogicalProcess
    {
    public:
        explicit LP2(warpsim::LPId id) : m_id(id)
        {
            m_types.register_trivial<A>(AType);
            m_types.register_trivial<B>(BType);

            // Low (t=5): write A only.
            m_dispatcher.register_trivial<std::uint32_t>(Low_WriteA,
                                                         [](warpsim::EntityId target, const std::uint32_t &, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                         {
                                                             auto &a = world.write<A>(target, AType);
                                                             a.value += 1;
                                                         });

            // High (t=10): write both A and B.
            m_dispatcher.register_trivial<std::uint32_t>(High_WriteBoth,
                                                         [this](warpsim::EntityId target, const std::uint32_t &, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                         {
                                                             auto &a = world.write<A>(target, AType);
                                                             a.value += 10;

                                                             auto &b = world.write<B>(b_id_(), BType);
                                                             b.value += 100;
                                                         });
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &) override
        {
            m_world.emplace<A>(a_id_(), AType, A{});
            m_world.emplace<B>(b_id_(), BType, B{});
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            warpsim::WorldTxn txn(m_world, ctx);
            m_dispatcher.dispatch(ev, ctx, txn);
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

        bool supports_entity_snapshots() const override { return true; }
        warpsim::ByteBuffer save_entity(warpsim::EntityId id) const override { return m_world.serialize_entity(id); }
        void load_entity(warpsim::EntityId id, std::span<const std::byte> bytes) override { m_world.deserialize_entity(id, bytes); }

        std::uint64_t a_value() const { return m_world.read<A>(a_id_(), AType).value; }
        std::uint64_t b_value() const { return m_world.read<B>(b_id_(), BType).value; }

    private:
        static constexpr warpsim::EntityTypeId AType = 7001;
        static constexpr warpsim::EntityTypeId BType = 7002;

        static constexpr warpsim::EntityId make_id_(warpsim::LPId owner, std::uint32_t local)
        {
            return (static_cast<warpsim::EntityId>(owner) << 32) | static_cast<warpsim::EntityId>(local);
        }

        warpsim::EntityId a_id_() const { return make_id_(m_id, 1); }
        warpsim::EntityId b_id_() const { return make_id_(m_id, 2); }

        warpsim::LPId m_id = 0;
        warpsim::EntityTypeRegistry m_types;
        warpsim::World m_world{m_types};
        warpsim::EventDispatcher<warpsim::WorldTxn> m_dispatcher;
    };
}

int main()
{
    auto transport = std::make_shared<ThrottledInProcTransport>();
    warpsim::Simulation sim(warpsim::SimulationConfig{.rank = 0}, transport);

    auto lp1 = std::make_unique<LP1>(1, 2);
    auto lp2 = std::make_unique<LP2>(2);

    auto *lp2Ptr = lp2.get();

    sim.add_lp(std::move(lp1));
    sim.add_lp(std::move(lp2));

    sim.run();

    // Correct final state is as-if executed in timestamp order:
    // t=5: A += 1
    // t=10: A += 10, B += 100
    assert(lp2Ptr->a_value() == 11);
    assert(lp2Ptr->b_value() == 100);

    return 0;
}
