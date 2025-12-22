#include "modeling.hpp"
#include "optimistic_migration.hpp"
#include "directory_lp.hpp"
#include "simulation.hpp"
#include "transport.hpp"
#include "world.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace
{
    using EventKind = std::uint32_t;

    // Model event kinds.
    inline constexpr EventKind KindTick = 51001;
    inline constexpr EventKind KindCommittedLog = 51002;

    // One simple entity we want to model.
    inline constexpr warpsim::EntityTypeId ThingType = 9001;

    struct Thing
    {
        std::uint64_t ticks = 0;
    };

    struct TickArgs
    {
        std::uint32_t unused = 0;
    };

    struct CommittedLog
    {
        warpsim::EntityId entity = 0;
        warpsim::LPId lp = 0;
        std::uint64_t ticks = 0;
    };

    // Minimal in-proc transport.
    class InProcTransport final : public warpsim::ITransport
    {
    public:
        void send(warpsim::WireMessage msg) override { m_queue.push_back(std::move(msg)); }

        std::optional<warpsim::WireMessage> poll() override
        {
            if (m_queue.empty())
            {
                return std::nullopt;
            }
            auto msg = std::move(m_queue.front());
            m_queue.pop_front();
            return msg;
        }

        bool has_pending() const override { return !m_queue.empty(); }

    private:
        std::deque<warpsim::WireMessage> m_queue;
    };

    bool world_has_entity(const warpsim::World &world, warpsim::EntityId id)
    {
        try
        {
            (void)world.type_of(id);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    class EntityWorkerLP final : public warpsim::ILogicalProcess
    {
    public:
        EntityWorkerLP(warpsim::LPId id, warpsim::EntityId entity) : m_id(id), m_entity(entity), m_world(m_types)
        {
            m_types.register_trivial<Thing>(ThingType);
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Only LP1 starts with the entity present.
            if (m_id == 1)
            {
                m_world.emplace<Thing>(m_entity, ThingType, Thing{});

                // Start a chain of targeted events. Note: we do NOT choose dst.
                warpsim::modeling::send_targeted(sink,
                                                 warpsim::TimeStamp{1, 0},
                                                 /*src=*/m_id,
                                                 /*target=*/m_entity,
                                                 /*kind=*/KindTick,
                                                 TickArgs{});
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            // Directory migration protocol: install entity state on the new owner.
            if (ev.payload.kind == warpsim::optimistic_migration::DefaultInstallKind)
            {
                const auto [entity, state] = warpsim::optimistic_migration::decode_install(ev);
                if (entity != m_entity)
                {
                    return;
                }

                if (!world_has_entity(m_world, entity))
                {
                    m_world.emplace<Thing>(entity, ThingType, Thing{});
                }

                m_world.deserialize_entity(entity, state);
                return;
            }

            if (ev.payload.kind != KindTick)
            {
                throw std::runtime_error("EntityWorkerLP: unexpected event kind");
            }

            (void)warpsim::modeling::require<TickArgs>(ev, KindTick, "tick");

            // Update entity state.
            warpsim::WorldTxn txn(m_world, ctx);
            auto &thing = txn.write<Thing>(m_entity, ThingType);
            thing.ticks += 1;

            // Emit committed output so prints are rollback-safe.
            warpsim::modeling::emit_committed(ctx,
                                              ev.ts,
                                              KindCommittedLog,
                                              CommittedLog{.entity = m_entity, .lp = m_id, .ticks = thing.ticks});

            // Demonstrate extensible migration criteria: the model supplies a policy callback
            // that can use any signals it wants.
            const warpsim::optimistic_migration::DecideNewOwnerFn policy = [](const warpsim::optimistic_migration::MigrationInputs &in) -> std::optional<warpsim::LPId>
            {
                // Simple demo policy:
                // - when owner==1 and localWork reaches 2, migrate to LP2
                if (in.currentOwner == 1 && in.localWork == 2)
                {
                    return static_cast<warpsim::LPId>(2);
                }
                return std::nullopt;
            };

            // Only the current owner attempts to migrate, because it owns the state bytes.
            // (The directory will forward targeted events regardless.)
            {
                const warpsim::optimistic_migration::MigrationInputs in{
                    .entity = m_entity,
                    .currentOwner = m_id,
                    .self = m_id,
                    .now = ev.ts,
                    .localWork = thing.ticks,
                    .remoteWork = 0,
                };

                // Migration update takes effect at a future time; avoid same-timestamp ambiguity.
                const warpsim::TimeStamp effectiveTs{ev.ts.time + 2, 0};
                const auto bytes = m_world.serialize_entity(m_entity);

                (void)warpsim::optimistic_migration::maybe_migrate(ctx,
                                                                   effectiveTs,
                                                                   /*src=*/m_id,
                                                                   /*directoryLp=*/DirectoryId,
                                                                   /*updateOwnerKind=*/warpsim::optimistic_migration::DefaultUpdateOwnerKind,
                                                                   bytes,
                                                                   in,
                                                                   policy);
            }

            // Keep generating work until t=8.
            if (ev.ts.time < 8)
            {
                warpsim::modeling::send_targeted(ctx,
                                                 warpsim::TimeStamp{ev.ts.time + 1, 0},
                                                 /*src=*/m_id,
                                                 /*target=*/m_entity,
                                                 /*kind=*/KindTick,
                                                 TickArgs{});
            }
        }

        warpsim::ByteBuffer save_state() const override
        {
            // Fallback LP snapshot: keep it minimal for the example.
            if (!world_has_entity(m_world, m_entity))
            {
                return {};
            }
            const auto &thing = m_world.read<Thing>(m_entity, ThingType);
            return warpsim::bytes_from_trivially_copyable(thing);
        }

        void load_state(std::span<const std::byte> bytes) override
        {
            // Fallback LP snapshot restore.
            if (bytes.empty())
            {
                return;
            }
            Thing t{};
            if (bytes.size() == sizeof(Thing))
            {
                std::memcpy(&t, bytes.data(), sizeof(Thing));
            }
            if (!world_has_entity(m_world, m_entity))
            {
                m_world.emplace<Thing>(m_entity, ThingType, t);
            }
            else
            {
                // Best-effort: overwrite by deserialize path.
                const auto b = warpsim::bytes_from_trivially_copyable(t);
                m_world.deserialize_entity(m_entity, std::span<const std::byte>(b.data(), b.size()));
            }
        }

        static inline constexpr warpsim::LPId DirectoryId = 100;

    private:
        warpsim::LPId m_id = 0;
        warpsim::EntityId m_entity = 0;

        warpsim::EntityTypeRegistry m_types;
        warpsim::World m_world;
    };
}

int main()
{
    constexpr warpsim::EntityId ThingId = 0xC0FFEE;

    auto transport = std::make_shared<InProcTransport>();

    warpsim::SimulationConfig cfg;
    cfg.rank = 0;

    // Route all targeted events through a single directory LP.
    cfg.directoryForEntity = [](warpsim::EntityId) -> warpsim::LPId
    { return EntityWorkerLP::DirectoryId; };

    cfg.committedSink = [](warpsim::RankId, warpsim::LPId, warpsim::TimeStamp ts, const warpsim::Payload &p)
    {
        if (p.kind == KindCommittedLog && p.bytes.size() == sizeof(CommittedLog))
        {
            CommittedLog log{};
            std::memcpy(&log, p.bytes.data(), sizeof(CommittedLog));
            std::cout << "t=" << ts.time << " entity=" << log.entity << " lp=" << log.lp << " ticks=" << log.ticks << "\n";
            return;
        }

        if (p.kind == warpsim::optimistic_migration::DefaultMigrationLogKind)
        {
            const auto r = warpsim::optimistic_migration::decode_migration_log_payload(p);
            std::cout << "t=" << ts.time << " MIGRATE entity=" << r.entity << " " << r.oldOwner << "->" << r.newOwner
                      << (r.sentInstall ? " (install)" : "") << "\n";
            return;
        }
    };

    warpsim::Simulation sim(cfg, transport);

    // Directory LP: default owner is LP1 until an update event changes it.
    auto dirCfg = warpsim::optimistic_migration::make_default_directory_config(EntityWorkerLP::DirectoryId,
                                                                               /*stateEntityId=*/0xD1EC7001u);
    dirCfg.defaultOwner = [](warpsim::EntityId, warpsim::TimeStamp) -> warpsim::LPId
    { return 1; };

    sim.add_lp(std::make_unique<warpsim::DirectoryLP>(dirCfg));
    sim.add_lp(std::make_unique<EntityWorkerLP>(1, ThingId));
    sim.add_lp(std::make_unique<EntityWorkerLP>(2, ThingId));

    sim.run();

    return 0;
}
