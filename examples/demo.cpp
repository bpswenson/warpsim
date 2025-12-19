#include "event_dispatcher.hpp"
#include "directory_lp.hpp"
#include "optimistic_migration.hpp"
#include "simulation.hpp"
#include "transport.hpp"
#include "world.hpp"

#include <cstring>
#include <iostream>
#include <mutex>
#include <deque>

namespace
{
    using EventTypeId = std::uint32_t;

    enum : EventTypeId
    {
        Car_SetThrottle = 1001,
        Car_Accelerate = 1002,
        Car_Brake = 1003,

        Wheel_Puncture = 1101,
        Wheel_Inflate = 1102,
    };

    struct SetThrottleArgs
    {
        std::uint32_t throttle = 0; // 0..100
    };

    struct AccelerateArgs
    {
        std::uint64_t deltaSpeed = 0;
    };

    struct BrakeArgs
    {
        std::uint64_t deltaSpeed = 0;
    };

    struct InflateArgs
    {
        std::uint32_t pressure = 32;
    };

    struct Car
    {
        std::uint64_t speed = 0;
        warpsim::EntityId engine = 0;
        warpsim::EntityId wheels[4]{};
    };

    struct Engine
    {
        std::uint32_t throttle = 0;
        std::uint32_t temperature = 70;
    };

    struct Wheel
    {
        std::uint32_t pressure = 32;
        std::uint32_t health = 100;
    };

    // Demo transport: returns at most one message per poll_transport() call.
    // This simulates "late" arrival so the kernel must handle stragglers.
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

    class CarLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit CarLP(warpsim::LPId id, warpsim::LPId peer) : m_id(id), m_peer(peer)
        {
            m_types.register_trivial<Car>(CarType);
            m_types.register_trivial<Engine>(EngineType);
            m_types.register_trivial<Wheel>(WheelType);

            // Register event "functions".
            m_dispatcher.register_trivial<SetThrottleArgs>(Car_SetThrottle,
                                                           [this](warpsim::EntityId target, const SetThrottleArgs &args, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                           {
                                                               const auto &car = world.read<Car>(target, CarType);
                                                               auto &engine = world.write<Engine>(car.engine, EngineType);
                                                               engine.throttle = (args.throttle > 100) ? 100 : args.throttle;
                                                           });

            m_dispatcher.register_trivial<AccelerateArgs>(Car_Accelerate,
                                                          [this](warpsim::EntityId target, const AccelerateArgs &args, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                          {
                                                              const auto &carRO = world.read<Car>(target, CarType);
                                                              const auto &engineRO = world.read<Engine>(carRO.engine, EngineType);

                                                              const std::uint64_t scaled = (args.deltaSpeed * engineRO.throttle) / 100;
                                                              auto &car = world.write<Car>(target, CarType);
                                                              car.speed += scaled;

                                                              auto &engine = world.write<Engine>(car.engine, EngineType);
                                                              engine.temperature += static_cast<std::uint32_t>(scaled / 10);
                                                          });

            m_dispatcher.register_trivial<BrakeArgs>(Car_Brake,
                                                     [](warpsim::EntityId target, const BrakeArgs &args, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                     {
                                                         auto &car = world.write<Car>(target, CarType);
                                                         car.speed = (car.speed > args.deltaSpeed) ? (car.speed - args.deltaSpeed) : 0;
                                                     });

            m_dispatcher.register_trivial<std::uint32_t>(Wheel_Puncture,
                                                         [](warpsim::EntityId target, const std::uint32_t &, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                         {
                                                             auto &wheel = world.write<Wheel>(target, WheelType);
                                                             wheel.pressure = 0;
                                                             wheel.health = (wheel.health > 10) ? (wheel.health - 10) : 0;
                                                         });

            m_dispatcher.register_trivial<InflateArgs>(Wheel_Inflate,
                                                       [](warpsim::EntityId target, const InflateArgs &args, const warpsim::Event &, warpsim::IEventContext &, warpsim::WorldTxn &world)
                                                       {
                                                           auto &wheel = world.write<Wheel>(target, WheelType);
                                                           if (wheel.health == 0)
                                                           {
                                                               return;
                                                           }
                                                           wheel.pressure = args.pressure;
                                                       });
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Create car + sub-entities owned by this LP.
            const warpsim::EntityId carId = make_id_(m_id, 1);
            const warpsim::EntityId engineId = make_id_(m_id, 2);
            const warpsim::EntityId wheel0 = make_id_(m_id, 10);
            const warpsim::EntityId wheel1 = make_id_(m_id, 11);
            const warpsim::EntityId wheel2 = make_id_(m_id, 12);
            const warpsim::EntityId wheel3 = make_id_(m_id, 13);

            m_world.emplace<Engine>(engineId, EngineType, Engine{});
            m_world.emplace<Wheel>(wheel0, WheelType, Wheel{});
            m_world.emplace<Wheel>(wheel1, WheelType, Wheel{});
            m_world.emplace<Wheel>(wheel2, WheelType, Wheel{});
            m_world.emplace<Wheel>(wheel3, WheelType, Wheel{});

            Car car;
            car.engine = engineId;
            car.wheels[0] = wheel0;
            car.wheels[1] = wheel1;
            car.wheels[2] = wheel2;
            car.wheels[3] = wheel3;
            m_world.emplace<Car>(carId, CarType, car);

            // LP1 sends several messages in a specific order. The demo transport releases
            // them gradually, so LP2 may process later timestamps first, causing rollback.
            if (m_id == 1)
            {
                const warpsim::EntityId remoteCarId = make_id_(m_peer, 1);
                const warpsim::EntityId remoteWheel0 = make_id_(m_peer, 10);

                warpsim::Event e1;
                e1.ts = warpsim::TimeStamp{10, 0};
                e1.src = m_id;
                e1.dst = m_peer;
                e1.target = remoteCarId;
                e1.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Car_Accelerate, AccelerateArgs{.deltaSpeed = 111});
                sink.send(e1);

                warpsim::Event e2;
                e2.ts = warpsim::TimeStamp{12, 0};
                e2.src = m_id;
                e2.dst = m_peer;
                e2.target = remoteCarId;
                e2.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Car_Brake, BrakeArgs{.deltaSpeed = 50});
                sink.send(e2);

                warpsim::Event e3;
                e3.ts = warpsim::TimeStamp{8, 0};
                e3.src = m_id;
                e3.dst = m_peer;
                e3.target = remoteWheel0;
                e3.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Wheel_Puncture, std::uint32_t{0});
                sink.send(e3);

                // Straggler: earlier timestamp arrives after t=10 processing.
                warpsim::Event e4;
                e4.ts = warpsim::TimeStamp{5, 0};
                e4.src = m_id;
                e4.dst = m_peer;
                e4.target = remoteCarId;
                e4.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Car_SetThrottle, SetThrottleArgs{.throttle = 100});
                sink.send(e4);

                warpsim::Event e5;
                e5.ts = warpsim::TimeStamp{6, 0};
                e5.src = m_id;
                e5.dst = m_peer;
                e5.target = remoteWheel0;
                e5.payload = warpsim::EventDispatcher<warpsim::WorldTxn>::make_trivial_payload(Wheel_Inflate, InflateArgs{.pressure = 28});
                sink.send(e5);
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            warpsim::WorldTxn txn(m_world, ctx);
            m_dispatcher.dispatch(ev, ctx, txn);
        }

        warpsim::ByteBuffer save_state() const override
        {
            // Fallback LP snapshot.
            // For the demo we snapshot just the car struct. A real model would likely
            // snapshot multiple entities or avoid LP snapshots entirely.
            const warpsim::EntityId carId = make_id_(m_id, 1);
            const auto &car = m_world.read<Car>(carId, CarType);
            return warpsim::bytes_from_trivially_copyable(car);
        }

        void load_state(std::span<const std::byte> state) override
        {
            (void)state;
            // Not used in this demo (entity snapshots are enabled), but keep the hook.
        }

        bool supports_entity_snapshots() const override { return true; }

        warpsim::ByteBuffer save_entity(warpsim::EntityId id) const override
        {
            return m_world.serialize_entity(id);
        }

        void load_entity(warpsim::EntityId id, std::span<const std::byte> bytes) override
        {
            m_world.deserialize_entity(id, bytes);
        }

        std::uint64_t car_speed() const
        {
            const warpsim::EntityId carId = make_id_(m_id, 1);
            const auto &car = m_world.read<Car>(carId, CarType);
            return car.speed;
        }

        std::uint32_t engine_throttle() const
        {
            const auto &car = m_world.read<Car>(make_id_(m_id, 1), CarType);
            const auto &engine = m_world.read<Engine>(car.engine, EngineType);
            return engine.throttle;
        }

        std::uint32_t engine_temperature() const
        {
            const auto &car = m_world.read<Car>(make_id_(m_id, 1), CarType);
            const auto &engine = m_world.read<Engine>(car.engine, EngineType);
            return engine.temperature;
        }

        std::uint32_t wheel0_pressure() const
        {
            const auto &car = m_world.read<Car>(make_id_(m_id, 1), CarType);
            const auto &wheel = m_world.read<Wheel>(car.wheels[0], WheelType);
            return wheel.pressure;
        }

    private:
        static constexpr warpsim::EntityTypeId CarType = 2001;
        static constexpr warpsim::EntityTypeId EngineType = 2002;
        static constexpr warpsim::EntityTypeId WheelType = 2003;

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
}

int main()
{
    auto transport = std::make_shared<ThrottledInProcTransport>();

    warpsim::SimulationConfig cfg{.rank = 0};
    cfg.directoryForEntity = [](warpsim::EntityId) -> warpsim::LPId
    { return 3; };
    cfg.directoryFossilCollectKind = warpsim::optimistic_migration::DefaultFossilCollectKind;

    warpsim::Simulation sim(cfg, transport);

    // Directory LP forwards targeted events to an owner-at-time. For this demo, the
    // EntityId encoding includes the owner LP in the high 32 bits.
    static constexpr warpsim::EntityId kDirectoryState = 0xD1EC7000ULL;

    warpsim::DirectoryLPConfig dirCfg;
    dirCfg.id = 3;
    dirCfg.stateEntityId = kDirectoryState;
    dirCfg.defaultOwner = [](warpsim::EntityId e, warpsim::TimeStamp) -> warpsim::LPId
    { return static_cast<warpsim::LPId>(e >> 32); };
    dirCfg.fossilCollectKind = warpsim::optimistic_migration::DefaultFossilCollectKind;
    dirCfg.decodeFossilCollect = [](const warpsim::Event &ev) -> warpsim::TimeStamp
    { return warpsim::optimistic_migration::decode_fossil_collect_payload(ev); };
    sim.add_lp(std::make_unique<warpsim::DirectoryLP>(dirCfg));

    auto lp1 = std::make_unique<CarLP>(1, 2);
    auto lp2 = std::make_unique<CarLP>(2, 1);

    auto *lp2Ptr = lp2.get();
    sim.add_lp(std::move(lp1));
    sim.add_lp(std::move(lp2));

    sim.run();

    std::cout << "LP2 car speed=" << lp2Ptr->car_speed() << "\n";
    std::cout << "LP2 engine throttle=" << lp2Ptr->engine_throttle() << "\n";
    std::cout << "LP2 engine temperature=" << lp2Ptr->engine_temperature() << "\n";
    std::cout << "LP2 wheel0 pressure=" << lp2Ptr->wheel0_pressure() << "\n";
    return 0;
}
