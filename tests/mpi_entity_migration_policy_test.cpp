/*
Purpose: MPI rank-count invariance test for policy-driven entity migration.

What this tests:
- Targeted (entity-addressed) events are routed via the directory across ranks.
- A model-defined migration policy (DecideNewOwnerFn) can emit update-owner events.
- The resulting committed output hash is identical across different MPI rank counts.

This is intentionally minimal and deterministic (no RNG).
*/

#include "directory_lp.hpp"
#include "modeling.hpp"
#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include "optimistic_migration.hpp"
#include "random.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>

namespace
{
    using EventKind = std::uint32_t;

    constexpr warpsim::EntityId kEntity = 0x1234567890ABCDEFULL;

    // LP ids are fixed so the model behavior is rank-count independent.
    constexpr warpsim::LPId kDirectoryLp = 100;
    constexpr warpsim::LPId kDriverLp = 200;

    // Per-rank heartbeat LP ids (not part of the model; used only to prove each rank executes work).
    constexpr warpsim::LPId kHeartbeatBaseLp = 10'000;

    constexpr warpsim::LPId kOwner1 = 1;
    constexpr warpsim::LPId kOwner2 = 2;
    constexpr warpsim::LPId kOwner3 = 3;
    constexpr warpsim::LPId kOwner4 = 4;

    constexpr std::uint64_t kEndTime = 12;

    // Model kinds.
    constexpr EventKind KindTick = 88001;
    constexpr EventKind KindCommitted = 88002;
    constexpr EventKind KindRankAliveCommitted = 88003;
    constexpr EventKind KindHeartbeatPing = 88004;

    struct TickArgs
    {
        std::uint32_t unused = 0;
    };

    struct Thing
    {
        std::uint64_t ticks = 0;
    };

    struct Committed
    {
        warpsim::EntityId entity = 0;
        warpsim::LPId lp = 0;
        std::uint64_t ticks = 0;
        std::uint64_t t = 0;
    };

    struct RankAlive
    {
        warpsim::RankId rank = 0;
        warpsim::LPId lp = 0;
    };

    class HeartbeatLP final : public warpsim::ILogicalProcess
    {
    public:
        HeartbeatLP(warpsim::LPId id, warpsim::RankId rank) : m_id(id), m_rank(rank) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            warpsim::Event ping;
            ping.ts = warpsim::TimeStamp{0, 0};
            ping.src = m_id;
            ping.dst = m_id;
            ping.payload.kind = KindHeartbeatPing;
            sink.send(std::move(ping));
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind != KindHeartbeatPing)
            {
                return;
            }

            warpsim::modeling::emit_committed(ctx,
                                              ev.ts,
                                              KindRankAliveCommitted,
                                              RankAlive{.rank = m_rank, .lp = m_id});
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        warpsim::RankId m_rank = 0;
    };

    inline std::uint64_t mix64(std::uint64_t a, std::uint64_t b) noexcept
    {
        return warpsim::splitmix64(a ^ warpsim::splitmix64(b));
    }

    // A deterministic, model-defined migration policy.
    // It uses only the entity-local tick count and current owner.
    inline warpsim::optimistic_migration::DecideNewOwnerFn demo_policy()
    {
        return [](const warpsim::optimistic_migration::MigrationInputs &in) -> std::optional<warpsim::LPId>
        {
            // Migrate twice to ensure cross-rank install + continued targeted routing.
            // - after 2 ticks on owner1, migrate to owner3
            // - after 4 ticks total on owner3, migrate to owner2
            if (in.currentOwner == kOwner1 && in.localWork == 2)
            {
                return kOwner3;
            }
            if (in.currentOwner == kOwner3 && in.localWork == 4)
            {
                return kOwner2;
            }
            return std::nullopt;
        };
    }

    class OwnerLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit OwnerLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &) override
        {
            // Establish initial state only on the default owner.
            if (m_id == kOwner1)
            {
                m_has = true;
                m_thing = Thing{};
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == warpsim::optimistic_migration::DefaultInstallKind)
            {
                const auto [entity, bytes] = warpsim::optimistic_migration::decode_install(ev);
                if (entity != kEntity)
                {
                    return;
                }
                if (bytes.size() != sizeof(Thing))
                {
                    throw std::runtime_error("mpi_entity_migration_policy_test: install state size mismatch");
                }
                Thing t{};
                std::memcpy(&t, bytes.data(), sizeof(Thing));
                m_thing = t;
                m_has = true;
                return;
            }

            if (ev.payload.kind != KindTick)
            {
                return;
            }
            if (ev.target != kEntity)
            {
                return;
            }

            (void)warpsim::modeling::require<TickArgs>(ev, KindTick, "tick");

            if (!m_has)
            {
                throw std::runtime_error("mpi_entity_migration_policy_test: tick delivered to owner without state (missing install?)");
            }

            // Mutate entity state.
            ctx.request_write(kEntity);
            m_thing.ticks += 1;

            // Rollback-safe observation.
            warpsim::modeling::emit_committed(ctx,
                                              ev.ts,
                                              KindCommitted,
                                              Committed{.entity = kEntity, .lp = m_id, .ticks = m_thing.ticks, .t = ev.ts.time});

            // Policy-driven migration: the owner decides based on signals.
            const auto policy = demo_policy();
            const warpsim::optimistic_migration::MigrationInputs in{
                .entity = kEntity,
                .currentOwner = m_id,
                .self = m_id,
                .now = ev.ts,
                .localWork = m_thing.ticks,
                .remoteWork = 0,
            };

            const warpsim::TimeStamp effectiveTs{ev.ts.time + 2, 0};
            const auto state = warpsim::bytes_from_trivially_copyable(m_thing);
            (void)warpsim::optimistic_migration::maybe_migrate(ctx,
                                                               effectiveTs,
                                                               /*src=*/m_id,
                                                               /*directoryLp=*/kDirectoryLp,
                                                               /*updateOwnerKind=*/warpsim::optimistic_migration::DefaultUpdateOwnerKind,
                                                               std::span<const std::byte>(state.data(), state.size()),
                                                               in,
                                                               policy);

            if (ev.ts.time < kEndTime)
            {
                warpsim::modeling::send_targeted(ctx,
                                                 warpsim::TimeStamp{ev.ts.time + 1, 0},
                                                 /*src=*/m_id,
                                                 /*target=*/kEntity,
                                                 /*kind=*/KindTick,
                                                 TickArgs{});
            }
        }

        warpsim::ByteBuffer save_state() const override
        {
            // Snapshot minimal LP state.
            struct S
            {
                std::uint8_t has;
                Thing thing;
            };
            return warpsim::bytes_from_trivially_copyable(S{static_cast<std::uint8_t>(m_has ? 1 : 0), m_thing});
        }

        void load_state(std::span<const std::byte> state) override
        {
            struct S
            {
                std::uint8_t has;
                Thing thing;
            };
            if (state.size() != sizeof(S))
            {
                return;
            }
            S s{};
            std::memcpy(&s, state.data(), sizeof(S));
            m_has = (s.has != 0);
            m_thing = s.thing;
        }

    private:
        warpsim::LPId m_id = 0;
        bool m_has = false;
        Thing m_thing{};
    };

    class DriverLP final : public warpsim::ILogicalProcess
    {
    public:
        DriverLP() = default;

        warpsim::LPId id() const noexcept override { return kDriverLp; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Seed the first targeted tick from a fixed LP id.
            warpsim::modeling::send_targeted(sink,
                                             warpsim::TimeStamp{1, 0},
                                             /*src=*/kDriverLp,
                                             /*target=*/kEntity,
                                             /*kind=*/KindTick,
                                             TickArgs{});
        }

        void on_event(const warpsim::Event &, warpsim::IEventContext &) override {}

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}
    };
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);

    int rank = 0;
    int size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2)
    {
        MPI_Finalize();
        return 0;
    }

    std::uint64_t localCommittedHash = 0;
    std::uint64_t localRankAliveCount = 0;

    auto transport = std::make_shared<warpsim::MpiTransport>(MPI_COMM_WORLD);

    warpsim::SimulationConfig cfg;
    cfg.rank = static_cast<warpsim::RankId>(rank);
    cfg.lpToRank = [size](warpsim::LPId lp) -> warpsim::RankId
    {
        // Force heartbeat LP ids to map 1:1 to ranks, independent of modulo mapping.
        const warpsim::LPId hbBegin = kHeartbeatBaseLp;
        const warpsim::LPId hbEnd = kHeartbeatBaseLp + static_cast<warpsim::LPId>(size);
        if (lp >= hbBegin && lp < hbEnd)
        {
            return static_cast<warpsim::RankId>(static_cast<int>(lp - hbBegin));
        }
        return static_cast<warpsim::RankId>(static_cast<int>(lp % static_cast<warpsim::LPId>(size)));
    };

    cfg.gvtReduceMin = [](warpsim::TimeStamp local) -> warpsim::TimeStamp
    { return warpsim::mpi_allreduce_min_timestamp(MPI_COMM_WORLD, local); };

    cfg.anyRankHasWork = [](bool localHasWork) -> bool
    { return warpsim::mpi_allreduce_any_work(MPI_COMM_WORLD, localHasWork); };

    // Directory routing for targeted sends.
    cfg.directoryForEntity = [](warpsim::EntityId) -> warpsim::LPId
    { return kDirectoryLp; };

    // Keep it focused.
    cfg.directoryFossilCollectKind = 0;

    cfg.committedSink = [&localCommittedHash, &localRankAliveCount](warpsim::RankId, warpsim::LPId, warpsim::TimeStamp, const warpsim::Payload &p)
    {
        if (p.kind == KindRankAliveCommitted)
        {
            localRankAliveCount += 1;
            return;
        }

        if (p.kind != KindCommitted || p.bytes.size() != sizeof(Committed))
        {
            return;
        }

        Committed c{};
        std::memcpy(&c, p.bytes.data(), sizeof(Committed));

        std::uint64_t x = 0;
        x = mix64(x, static_cast<std::uint64_t>(c.entity));
        x = mix64(x, static_cast<std::uint64_t>(c.lp));
        x = mix64(x, c.ticks);
        x = mix64(x, c.t);
        localCommittedHash ^= warpsim::splitmix64(x);
    };

    warpsim::Simulation sim(cfg, transport);

    // Add a heartbeat LP on every rank.
    sim.add_lp(std::make_unique<HeartbeatLP>(kHeartbeatBaseLp + static_cast<warpsim::LPId>(rank), cfg.rank));

    // Add directory LP on its owning rank.
    if (cfg.lpToRank(kDirectoryLp) == cfg.rank)
    {
        auto dirCfg = warpsim::optimistic_migration::make_default_directory_config(kDirectoryLp,
                                                                                   /*stateEntityId=*/0xD1EC7002u);
        dirCfg.defaultOwner = [](warpsim::EntityId, warpsim::TimeStamp) -> warpsim::LPId
        { return kOwner1; };
        sim.add_lp(std::make_unique<warpsim::DirectoryLP>(dirCfg));
    }

    // Add four owners (distributed by lpToRank).
    for (warpsim::LPId lp : {kOwner1, kOwner2, kOwner3, kOwner4})
    {
        if (cfg.lpToRank(lp) == cfg.rank)
        {
            sim.add_lp(std::make_unique<OwnerLP>(lp));
        }
    }

    // Add the driver on its owning rank.
    if (cfg.lpToRank(kDriverLp) == cfg.rank)
    {
        sim.add_lp(std::make_unique<DriverLP>());
    }

    sim.run();

    // Ensure every rank produced at least one local 'alive' committed record.
    std::uint64_t aliveMin = 0;
    MPI_Allreduce(&localRankAliveCount, &aliveMin, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);

    std::uint64_t globalHash = 0;
    MPI_Allreduce(&localCommittedHash, &globalHash, 1, MPI_UINT64_T, MPI_BXOR, MPI_COMM_WORLD);

    // Single expected value, stable across rank counts.
    constexpr std::uint64_t kExpectedGlobalHash = 17753189643905865615ULL;

    int rc = 0;
    if (rank == 0)
    {
        if (aliveMin == 0)
        {
            std::cerr << "[mpi_entity_migration_policy_test] gimp-check failed: some rank produced no local work\n";
            rc = 2;
        }
        if (globalHash != kExpectedGlobalHash)
        {
            std::cerr << "[mpi_entity_migration_policy_test] verify failed: expected=" << kExpectedGlobalHash << " got=" << globalHash << "\n";
            rc = 1;
        }
    }

    MPI_Finalize();
    return rc;
}
