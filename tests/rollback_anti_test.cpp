#include "event_dispatcher.hpp"
#include "simulation.hpp"
#include "world.hpp"

#include <cassert>
#include <cstdint>
#include <memory>

namespace
{
    using EventTypeId = std::uint32_t;

    enum : EventTypeId
    {
        // LP2 -> LP1
        Ping_Increment = 2001,

        // LP1 -> LP2 (causes rollback)
        Config_SetThrottle = 3001,
        Car_Accelerate = 3002,
    };

    struct SetThrottleArgs
    {
        std::uint32_t throttle = 0;
    };

    struct AccelerateArgs
    {
        std::uint64_t deltaSpeed = 0;
    };

    struct Counter
    {
        std::uint64_t value = 0;
    };

    struct Engine
    {
        std::uint32_t throttle = 0;
    };

    struct Car
    {
        std::uint64_t speed = 0;
        warpsim::EntityId engine = 0;
    };

    class LP1 final : public warpsim::ILogicalProcess
    {
    public:
        explicit LP1(warpsim::LPId id, warpsim::LPId peer) : m_id(id), m_peer(peer)
        {
            m_types.register_trivial<Counter>(CounterType);

            // Ping from LP2 should be cancelled by anti-message after rollback.
            m_dispatcher.register_trivial<std::uint32_t>(Ping_Increment,
                                                         [](warpsim::EntityId target, const std::uint32_t &, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                         {
                                                             auto &c = world.write<Counter>(target, CounterType);
                                                             c.value += 1;
                                                         });
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Create local counter entity.
            m_world.emplace<Counter>(counter_id_(), CounterType, Counter{});

            // Seed LP2 with a high-time event that will cause it to send us a ping.
            if (m_id == 1)
            {
                const warpsim::EntityId remoteCarId = make_id_(m_peer, 1);

                warpsim::Event e_high;
                e_high.ts = warpsim::TimeStamp{10, 0};
                e_high.src = m_id;
                e_high.dst = m_peer;
                e_high.target = remoteCarId;
                e_high.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Car_Accelerate, AccelerateArgs{.deltaSpeed = 100});
                sink.send(e_high);

                // Straggler: arrives after LP2 processed t=10.
                warpsim::Event e_low;
                e_low.ts = warpsim::TimeStamp{5, 0};
                e_low.src = m_id;
                e_low.dst = m_peer;
                e_low.target = remoteCarId;
                e_low.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Config_SetThrottle, SetThrottleArgs{.throttle = 100});
                sink.send(e_low);
            }
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

        std::uint64_t counter_value() const
        {
            return m_world.read<Counter>(counter_id_(), CounterType).value;
        }

    private:
        static constexpr warpsim::EntityTypeId CounterType = 5001;

        static constexpr warpsim::EntityId make_id_(warpsim::LPId owner, std::uint32_t local)
        {
            return (static_cast<warpsim::EntityId>(owner) << 32) | static_cast<warpsim::EntityId>(local);
        }

        warpsim::EntityId counter_id_() const { return make_id_(m_id, 42); }

        warpsim::LPId m_id = 0;
        warpsim::LPId m_peer = 0;
        warpsim::EntityTypeRegistry m_types;
        warpsim::World m_world{m_types};
        warpsim::EventDispatcher<warpsim::WorldTxn> m_dispatcher;
    };

    class LP2 final : public warpsim::ILogicalProcess
    {
    public:
        explicit LP2(warpsim::LPId id, warpsim::LPId peer) : m_id(id), m_peer(peer)
        {
            m_types.register_trivial<Car>(CarType);
            m_types.register_trivial<Engine>(EngineType);

            // Straggler config (t=5) updates engine throttle.
            m_dispatcher.register_trivial<SetThrottleArgs>(Config_SetThrottle,
                                                           [](warpsim::EntityId target, const SetThrottleArgs &args, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                           {
                                                               const auto &car = world.read<Car>(target, CarType);
                                                               auto &engine = world.write<Engine>(car.engine, EngineType);
                                                               engine.throttle = args.throttle;
                                                           });

            // High-time event (t=10) sends a ping to LP1 at t=11.
            // After rollback to t=5, the send must be cancelled via anti-message.
            m_dispatcher.register_trivial<AccelerateArgs>(Car_Accelerate,
                                                          [this](warpsim::EntityId target, const AccelerateArgs &, const warpsim::Event &ev, warpsim::IEventContext &ctx, warpsim::WorldTxn &world)
                                                          {
                                                              const auto &car = world.read<Car>(target, CarType);
                                                              const auto &engine = world.read<Engine>(car.engine, EngineType);

                                                              // Before the straggler config (t=5) is applied, throttle is 0 so we send.
                                                              // After rollback+replay, throttle becomes 100 so the send must not recur.
                                                              if (engine.throttle != 0)
                                                              {
                                                                  return;
                                                              }

                                                              warpsim::Event ping;
                                                              ping.ts = warpsim::TimeStamp{ev.ts.time + 1, 0};
                                                              ping.src = m_id;
                                                              ping.dst = m_peer;
                                                              ping.target = make_id_(m_peer, 42);
                                                              ping.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Ping_Increment, std::uint32_t{0});
                                                              ctx.send(ping);
                                                          });
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &) override
        {
            const warpsim::EntityId carId = make_id_(m_id, 1);
            const warpsim::EntityId engineId = make_id_(m_id, 2);
            m_world.emplace<Engine>(engineId, EngineType, Engine{});
            m_world.emplace<Car>(carId, CarType, Car{.speed = 0, .engine = engineId});
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

    private:
        static constexpr warpsim::EntityTypeId CarType = 6001;
        static constexpr warpsim::EntityTypeId EngineType = 6002;

        static constexpr warpsim::EntityId make_id_(warpsim::LPId owner, std::uint32_t local)
        {
            return (static_cast<warpsim::EntityId>(owner) << 32) | static_cast<warpsim::EntityId>(local);
        }

        warpsim::LPId m_id = 0;
        warpsim::LPId m_peer = 0;
        warpsim::EntityTypeRegistry m_types;
        warpsim::World m_world{m_types};
        warpsim::EventDispatcher<warpsim::WorldTxn> m_dispatcher;
    };

    // Transport that drips messages so a straggler arrives "late".
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
}

int main()
{
    auto transport = std::make_shared<ThrottledInProcTransport>();
    warpsim::Simulation sim(warpsim::SimulationConfig{.rank = 0}, transport);

    auto lp1 = std::make_unique<LP1>(1, 2);
    auto lp2 = std::make_unique<LP2>(2, 1);

    auto *lp1Ptr = lp1.get();

    sim.add_lp(std::move(lp1));
    sim.add_lp(std::move(lp2));

    sim.run();

    // If anti-messages + cancellation works, LP1 should not have applied the ping.
    // Without anti-cancel, counter would become 1.
    assert(lp1Ptr->counter_value() == 0);

    return 0;
}
