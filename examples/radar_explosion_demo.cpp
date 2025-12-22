#include "aoi_lp.hpp"
#include "aoi_client.hpp"
#include "event_dispatcher.hpp"
#include "random.hpp"
#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

namespace
{
    using EventTypeId = std::uint32_t;

    enum : EventTypeId
    {
        // AOI protocol kinds for this demo.
        Aoi_Update = 7001,
        Aoi_Query = 7002,
        Aoi_Result = 7003,

        // Model-level events.
        Driver_Tick = 7101,
        Aircraft_Move = 7201,
        Radar_Ping = 7301,
        Explosion_Detonate = 7401,
        Radar_Contacts = 7501,
        Explosion_Affect = 7601,
    };

    struct MoveArgs
    {
        double dx = 0;
        double dy = 0;
        double dz = 0;
    };

    struct ContactsArgs
    {
        std::uint64_t requestId = 0;
        std::uint32_t count = 0;
    };

    struct AffectArgs
    {
        std::uint64_t requestId = 0;
        std::uint32_t count = 0;
    };

    static constexpr warpsim::EntityId make_id(warpsim::LPId owner, std::uint32_t local)
    {
        return (static_cast<warpsim::EntityId>(owner) << 32) | static_cast<warpsim::EntityId>(local);
    }

    // Demo transport: in-process FIFO delivery.
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
            if (m_queue.empty())
            {
                return std::nullopt;
            }

            auto msg = std::move(m_queue.front());
            m_queue.pop_front();
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
    };

    struct Aircraft
    {
        warpsim::Vec3 pos{};
        std::uint64_t radarContactCount = 0;
        std::uint64_t explosionAffectedCount = 0;
    };

    class AircraftLP final : public warpsim::ILogicalProcess
    {
    public:
        AircraftLP(warpsim::LPId id, warpsim::LPId aoiLp) : m_id(id), m_aoi(aoiLp)
        {
            m_aoiClient.aoiLp = aoiLp;
            m_aoiClient.updateKind = Aoi_Update;
            m_aoiClient.queryKind = Aoi_Query;

            m_state.pos = (m_id == 1) ? warpsim::Vec3{0, 0, 0} : warpsim::Vec3{1200, 0, 0};

            m_dispatcher.register_trivial<MoveArgs>(Aircraft_Move,
                                                    [this](warpsim::EntityId target, const MoveArgs &a, const warpsim::Event &ev, warpsim::IEventContext &ctx, Aircraft &ac)
                                                    {
                                                        (void)target;
                                                        // Snapshot before mutation.
                                                        ctx.request_write(kStateEntity);
                                                        ac.pos.x += a.dx;
                                                        ac.pos.y += a.dy;
                                                        ac.pos.z += a.dz;

                                                        publish_pos_(ctx, ev.ts, ac.pos);
                                                    });

            m_dispatcher.register_handler(Radar_Contacts,
                                          [this](const warpsim::Event &ev, warpsim::IEventContext &ctx, Aircraft &ac)
                                          {
                                              (void)ev;
                                              ctx.request_write(kStateEntity);
                                              ++ac.radarContactCount;
                                          });

            m_dispatcher.register_handler(Explosion_Affect,
                                          [this](const warpsim::Event &ev, warpsim::IEventContext &ctx, Aircraft &ac)
                                          {
                                              (void)ev;
                                              ctx.request_write(kStateEntity);
                                              ++ac.explosionAffectedCount;
                                          });

            m_dispatcher.register_handler(Driver_Tick,
                                          [](const warpsim::Event &, warpsim::IEventContext &, Aircraft &) {});
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            (void)sink;
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            m_dispatcher.dispatch(ev, ctx, m_state);
        }

        warpsim::ByteBuffer save_state() const override
        {
            return warpsim::bytes_from_trivially_copyable(m_state);
        }

        void load_state(std::span<const std::byte> bytes) override
        {
            if (bytes.size() != sizeof(Aircraft))
            {
                throw std::runtime_error("AircraftLP: bad state");
            }
            std::memcpy(&m_state, bytes.data(), sizeof(Aircraft));
        }

        warpsim::Vec3 pos() const { return m_state.pos; }
        std::uint64_t radar_contacts() const { return m_state.radarContactCount; }
        std::uint64_t explosion_hits() const { return m_state.explosionAffectedCount; }

        warpsim::EntityId aircraft_entity() const { return make_id(m_id, 1); }

    private:
        void publish_pos_(warpsim::IEventContext &ctx, warpsim::TimeStamp ts, warpsim::Vec3 pos)
        {
            m_aoiClient.send_update(ctx, ts, m_id, aircraft_entity(), pos);
        }

        static constexpr warpsim::EntityId kStateEntity = 0xACACAC01ULL;

        warpsim::LPId m_id = 0;
        warpsim::LPId m_aoi = 0;
        warpsim::AoiClient m_aoiClient;
        warpsim::EventDispatcher<Aircraft> m_dispatcher;
        Aircraft m_state{};
    };

    class DriverLP final : public warpsim::ILogicalProcess
    {
    public:
        DriverLP(warpsim::LPId id, warpsim::LPId aoi, warpsim::LPId shooterLp, warpsim::LPId targetLp, std::uint64_t seed)
            : m_id(id), m_aoi(aoi), m_shooterLp(shooterLp), m_targetLp(targetLp), m_seed(seed)
        {
            m_aoiClient.aoiLp = aoi;
            m_aoiClient.updateKind = Aoi_Update;
            m_aoiClient.queryKind = Aoi_Query;
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Seed ticks.
            for (std::uint64_t t = 1; t <= 20; ++t)
            {
                warpsim::Event tick;
                tick.ts = warpsim::TimeStamp{t, 0};
                tick.src = m_id;
                tick.dst = m_id;
                tick.payload.kind = Driver_Tick;
                sink.send(std::move(tick));
            }

            // Move target aircraft closer.
            send_move_(sink, warpsim::TimeStamp{5, 0}, m_targetLp, MoveArgs{-400.0, 0.0, 0.0});
            send_move_(sink, warpsim::TimeStamp{10, 0}, m_targetLp, MoveArgs{-200.0, 0.0, 0.0});
            send_move_(sink, warpsim::TimeStamp{12, 0}, m_targetLp, MoveArgs{-200.0, 0.0, 0.0});

            // Radar ping at t=11.
            {
                warpsim::Event ping;
                ping.ts = warpsim::TimeStamp{11, 0};
                ping.src = m_id;
                ping.dst = m_id;
                ping.payload.kind = Radar_Ping;
                sink.send(std::move(ping));
            }

            // Explosion at t=13.
            {
                warpsim::Event boom;
                boom.ts = warpsim::TimeStamp{13, 0};
                boom.src = m_id;
                boom.dst = m_id;
                boom.payload.kind = Explosion_Detonate;
                sink.send(std::move(boom));
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == Driver_Tick)
            {
                return;
            }

            if (ev.payload.kind == Radar_Ping)
            {
                // Cone query in +X with 60deg half-angle, range 1500.
                warpsim::AoiLP::QueryArgs q;
                q.requestId = (1ULL << 62) | (static_cast<std::uint64_t>(ev.uid) & ((1ULL << 62) - 1));
                q.self = make_id(m_shooterLp, 1);
                q.shape = warpsim::AoiLP::QueryShape::Cone;
                q.origin = warpsim::Vec3{0, 0, 0};
                q.direction = warpsim::Vec3{1, 0, 0};
                q.range = 1500.0;
                q.cosHalfAngle = std::cos(60.0 * 3.141592653589793 / 180.0);

                m_aoiClient.send_query(ctx, ev.ts, m_id, q);
                return;
            }

            if (ev.payload.kind == Explosion_Detonate)
            {
                // Sphere query centered near shooter, radius 600.
                warpsim::AoiLP::QueryArgs q;
                q.requestId = (2ULL << 62) | (static_cast<std::uint64_t>(ev.uid) & ((1ULL << 62) - 1));
                q.self = 0;
                q.shape = warpsim::AoiLP::QueryShape::Sphere;

                // Deterministic jitter to demonstrate rollback-safe RNG.
                // Keyed off (seed, LPId, event uid) so it doesn't depend on rank or execution order.
                const double u = warpsim::rng_unit_double(m_seed, m_id, /*stream=*/1, ev.src, ev.uid, /*draw=*/0);
                const double jitter = (u * 2.0 - 1.0) * 10.0; // [-10,+10]
                q.origin = warpsim::Vec3{200.0 + jitter, 0, 0};
                q.radius = 600.0;

                m_aoiClient.send_query(ctx, ev.ts, m_id, q);
                return;
            }

            if (ev.payload.kind == Aoi_Result)
            {
                const auto res = warpsim::AoiClient::decode_result(ev.payload);

                const std::uint64_t tag = res.requestId >> 62;
                if (tag == 1)
                {
                    for (std::size_t i = 0; i < res.entities.size(); ++i)
                    {
                        warpsim::Event out;
                        out.ts = ev.ts;
                        out.src = m_id;
                        out.dst = m_shooterLp;
                        out.target = 0;
                        out.payload.kind = Radar_Contacts;
                        out.payload.bytes = warpsim::bytes_from_trivially_copyable(ContactsArgs{.requestId = res.requestId, .count = 1});
                        ctx.send(std::move(out));
                    }
                    return;
                }

                if (tag == 2)
                {
                    for (std::size_t i = 0; i < res.entities.size(); ++i)
                    {
                        const warpsim::EntityId e = res.entities[i];
                        const warpsim::LPId owner = static_cast<warpsim::LPId>(e >> 32);

                        warpsim::Event out;
                        out.ts = ev.ts;
                        out.src = m_id;
                        out.dst = owner;
                        out.target = 0;
                        out.payload.kind = Explosion_Affect;
                        out.payload.bytes = warpsim::bytes_from_trivially_copyable(AffectArgs{.requestId = res.requestId, .count = 1});
                        ctx.send(std::move(out));
                    }
                    return;
                }

                return;
            }
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        void send_move_(warpsim::IEventSink &sink, warpsim::TimeStamp ts, warpsim::LPId dstLp, MoveArgs a)
        {
            warpsim::Event ev;
            ev.ts = ts;
            ev.src = m_id;
            ev.dst = dstLp;
            ev.payload.kind = Aircraft_Move;
            ev.payload.bytes = warpsim::bytes_from_trivially_copyable(a);
            sink.send(std::move(ev));
        }

        warpsim::LPId m_id = 0;
        warpsim::LPId m_aoi = 0;
        warpsim::AoiClient m_aoiClient;
        warpsim::LPId m_shooterLp = 0;
        warpsim::LPId m_targetLp = 0;
        std::uint64_t m_seed = 1;
    };
}

int main()
{
    auto transport = std::make_shared<ThrottledInProcTransport>();
    warpsim::SimulationConfig cfg;
    cfg.rank = 0;
    cfg.seed = 1;
    warpsim::Simulation sim(cfg, transport);

    // AOI LP (id=10).
    warpsim::AoiLP::Config aoiCfg;
    aoiCfg.id = 10;
    aoiCfg.stateEntityId = 0xA01A01ULL;
    aoiCfg.updateKind = Aoi_Update;
    aoiCfg.queryKind = Aoi_Query;
    aoiCfg.resultKind = Aoi_Result;
    sim.add_lp(std::make_unique<warpsim::AoiLP>(aoiCfg));

    auto shooter = std::make_unique<AircraftLP>(1, /*aoiLp=*/10);
    auto target = std::make_unique<AircraftLP>(2, /*aoiLp=*/10);

    auto *shooterP = shooter.get();
    auto *targetP = target.get();

    sim.add_lp(std::move(shooter));
    sim.add_lp(std::move(target));

    // Driver emits pings/explosions and consumes AOI results.
    sim.add_lp(std::make_unique<DriverLP>(3, /*aoi=*/10, /*shooterLp=*/1, /*targetLp=*/2, cfg.seed));

    // Deterministically seed AOI positions before any LP on_start() runs.
    warpsim::AoiClient aoiClient;
    aoiClient.aoiLp = 10;
    aoiClient.updateKind = Aoi_Update;
    aoiClient.queryKind = Aoi_Query;
    {
        aoiClient.send_update(sim, warpsim::TimeStamp{0, 0}, /*srcLp=*/1, shooterP->aircraft_entity(), shooterP->pos());
    }
    {
        aoiClient.send_update(sim, warpsim::TimeStamp{0, 0}, /*srcLp=*/2, targetP->aircraft_entity(), targetP->pos());
    }

    sim.run();

    // Sanity: shooter should have received at least one contact notification,
    // and at least one aircraft should have been affected by the explosion.
    assert(shooterP->radar_contacts() >= 1);
    assert(shooterP->explosion_hits() + targetP->explosion_hits() >= 1);

    std::cout << "radar_explosion_demo ok\n";
    std::cout << "shooter contacts=" << shooterP->radar_contacts() << " explosionHits=" << shooterP->explosion_hits() << "\n";
    std::cout << "target explosionHits=" << targetP->explosion_hits() << "\n";
    return 0;
}
