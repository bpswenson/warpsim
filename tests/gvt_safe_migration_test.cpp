/*
Purpose: Validates that entity migration is safe with respect to GVT/commit.

What this tests: After a migration is committed, messages sent to the old owner are
re-routed to the new owner, and entity state/use events still behave correctly.
*/

#include "migration.hpp"
#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

namespace
{
    static constexpr std::uint32_t TickKind = 1001;
    static constexpr std::uint32_t UseEntityKind = 1002;

    struct EntityState
    {
        std::uint64_t value = 0;
    };

    class MigrationLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit MigrationLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == TickKind)
            {
                // At a known logical time, emit an event targeting the entity.
                // Intentionally set dst to the *old* owner (LP0); Simulation::send
                // should override dst to the migrated owner once the migration is committed.
                if (m_id == 0 && ev.ts.time == 7)
                {
                    warpsim::Event out;
                    out.ts = warpsim::TimeStamp{8, 0};
                    out.src = m_id;
                    out.dst = 0; // wrong on purpose
                    out.target = kEntity;
                    out.payload.kind = UseEntityKind;
                    ctx.send(std::move(out));
                }
                return;
            }

            if (ev.payload.kind == UseEntityKind)
            {
                if (ev.target != kEntity)
                {
                    throw std::runtime_error("unexpected target");
                }
                m_sawUseEntity = true;
                return;
            }

            throw std::runtime_error("unexpected event kind");
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

        void on_migration(warpsim::EntityId entity, std::span<const std::byte> bytes) override
        {
            if (entity != kEntity)
            {
                throw std::runtime_error("unexpected entity");
            }
            if (bytes.size() != sizeof(EntityState))
            {
                throw std::runtime_error("bad entity state size");
            }
            EntityState st{};
            std::memcpy(&st, bytes.data(), sizeof(EntityState));
            m_migratedValue = st.value;
            m_sawMigration = true;
        }

        bool saw_migration() const { return m_sawMigration; }
        std::uint64_t migrated_value() const { return m_migratedValue; }
        bool saw_use_entity() const { return m_sawUseEntity; }

        static constexpr warpsim::EntityId kEntity = 0xABCDEF01ULL;

    private:
        warpsim::LPId m_id = 0;
        bool m_sawMigration = false;
        std::uint64_t m_migratedValue = 0;
        bool m_sawUseEntity = false;
    };
}

int main()
{
    auto transport = std::make_shared<warpsim::InProcTransport>();

    warpsim::SimulationConfig cfg;
    cfg.collectivePeriodIters = 1;

    warpsim::Simulation sim(cfg, transport);

    auto lp0 = std::make_unique<MigrationLP>(0);
    auto lp1 = std::make_unique<MigrationLP>(1);
    auto *lp0Ptr = lp0.get();
    auto *lp1Ptr = lp1.get();

    sim.add_lp(std::move(lp0));
    sim.add_lp(std::move(lp1));

    // Seed tick events for both LPs to advance GVT beyond the migration cutover time.
    for (std::uint64_t t = 1; t <= 9; ++t)
    {
        warpsim::Event e0;
        e0.ts = warpsim::TimeStamp{t, 0};
        e0.src = 0;
        e0.dst = 0;
        e0.payload.kind = TickKind;
        sim.schedule(std::move(e0));

        warpsim::Event e1;
        e1.ts = warpsim::TimeStamp{t, 0};
        e1.src = 1;
        e1.dst = 1;
        e1.payload.kind = TickKind;
        sim.schedule(std::move(e1));
    }

    // Inject a migration message (effective at logical time 5) that transfers state to LP1.
    warpsim::Migration mig;
    mig.entity = MigrationLP::kEntity;
    mig.newOwner = 1;
    mig.effectiveTs = warpsim::TimeStamp{5, 0};
    mig.state = warpsim::bytes_from_trivially_copyable(EntityState{42});

    warpsim::WireMessage msg;
    msg.kind = warpsim::MessageKind::Migration;
    msg.srcRank = 0;
    msg.dstRank = 0;
    msg.bytes = warpsim::encode_migration(mig);
    transport->send(std::move(msg));

    sim.run();

    // Migration must be applied (GVT-safe commit).
    assert(lp1Ptr->saw_migration());
    assert(lp1Ptr->migrated_value() == 42);

    // The targeted event must be routed to the new owner.
    assert(!lp0Ptr->saw_use_entity());
    assert(lp1Ptr->saw_use_entity());

    return 0;
}
