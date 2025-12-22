/*
Purpose: End-to-end test of optimistic entity migration using a directory LP.

What this tests: Owner updates and installs are routed through the directory, events are
delivered to the correct owner-at-time, and the final owner/state are consistent.
*/

#include "directory_lp.hpp"
#include "optimistic_migration.hpp"
#include "simulation.hpp"
#include "transport.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace
{
    static constexpr std::uint32_t DriverTick = 2001;
    static constexpr std::uint32_t DirectoryUpdateOwner = 2002;
    static constexpr std::uint32_t InstallEntityState = 2003;
    static constexpr std::uint32_t UseEntity = 2004;

    struct UpdateOwnerArgs
    {
        warpsim::EntityId entity = 0;
        warpsim::LPId newOwner = 0;
        std::uint64_t value = 0;
    };

    struct InstallArgs
    {
        warpsim::EntityId entity = 0;
        std::uint64_t value = 0;
    };

    static warpsim::ByteBuffer pack_u32_u64(std::uint32_t kind, const warpsim::ByteBuffer &bytes)
    {
        (void)kind;
        return bytes;
    }

    static constexpr warpsim::EntityId kDirectoryState = 0xD1EC7000ULL;

    class OwnerLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit OwnerLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == InstallEntityState)
            {
                if (ev.payload.bytes.size() != sizeof(InstallArgs))
                {
                    throw std::runtime_error("InstallEntityState: bad payload");
                }
                InstallArgs ia{};
                std::memcpy(&ia, ev.payload.bytes.data(), sizeof(InstallArgs));
                if (ia.entity != kEntity)
                {
                    throw std::runtime_error("InstallEntityState: wrong entity");
                }

                // Ensure Time Warp snapshots our state before mutation.
                ctx.request_write(kOwnerState);
                m_installedValue = ia.value;
                return;
            }

            if (ev.payload.kind == UseEntity)
            {
                if (ev.target != kEntity)
                {
                    throw std::runtime_error("UseEntity: wrong target");
                }

                // Ensure Time Warp snapshots our state before mutation.
                ctx.request_write(kOwnerState);
                ++m_useCount;
                return;
            }

            if (ev.payload.kind == DriverTick)
            {
                return;
            }

            throw std::runtime_error("OwnerLP: unexpected kind");
        }

        warpsim::ByteBuffer save_state() const override
        {
            struct S
            {
                std::uint64_t installedValue;
                std::uint64_t useCount;
            };
            return warpsim::bytes_from_trivially_copyable(S{m_installedValue, m_useCount});
        }

        void load_state(std::span<const std::byte> state) override
        {
            struct S
            {
                std::uint64_t installedValue;
                std::uint64_t useCount;
            };
            if (state.size() != sizeof(S))
            {
                throw std::runtime_error("OwnerLP: bad state size");
            }
            S s{};
            std::memcpy(&s, state.data(), sizeof(S));
            m_installedValue = s.installedValue;
            m_useCount = s.useCount;
        }

        std::uint64_t installed_value() const { return m_installedValue; }
        std::uint64_t use_count() const { return m_useCount; }

        static constexpr warpsim::EntityId kEntity = 0x1111222233334444ULL;
        static constexpr warpsim::EntityId kOwnerState = 0x0ABCDEF0ULL;

    private:
        warpsim::LPId m_id = 0;
        std::uint64_t m_installedValue = 0;
        std::uint64_t m_useCount = 0;
    };

    class DriverLP final : public warpsim::ILogicalProcess
    {
    public:
        DriverLP(warpsim::LPId id, warpsim::LPId directory) : m_id(id), m_directory(directory) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind != DriverTick)
            {
                throw std::runtime_error("DriverLP: unexpected kind");
            }

            // At logical time 6: send a targeted event at time 7 (will be routed to directory).
            if (ev.ts.time == 6)
            {
                warpsim::Event use;
                use.ts = warpsim::TimeStamp{7, 0};
                use.src = m_id;
                use.dst = 0; // irrelevant; Simulation routes by target
                use.target = OwnerLP::kEntity;
                use.payload.kind = UseEntity;
                ctx.send(std::move(use));
                return;
            }

            // At logical time 8: send a straggler migration update with timestamp 5.
            // This arrives after the directory processed the event at t=7, forcing rollback.
            if (ev.ts.time == 8)
            {
                UpdateOwnerArgs a{OwnerLP::kEntity, /*newOwner=*/1, /*value=*/99};
                warpsim::Event upd;
                upd.ts = warpsim::TimeStamp{5, 0};
                upd.src = m_id;
                upd.dst = m_directory; // send directly to directory
                upd.target = OwnerLP::kEntity;
                upd.payload.kind = DirectoryUpdateOwner;
                upd.payload.bytes = warpsim::bytes_from_trivially_copyable(a);
                ctx.send(std::move(upd));
                return;
            }
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
        warpsim::LPId m_directory = 0;
    };
}

int main()
{
    auto transport = std::make_shared<warpsim::InProcTransport>();

    warpsim::SimulationConfig cfg;
    cfg.collectivePeriodIters = 1;

    // Use a single directory LP with id=2.
    cfg.directoryForEntity = [](warpsim::EntityId) -> warpsim::LPId
    { return 2; };

    // Enable kernel-driven GVT notifications so the directory can fossil-collect.
    cfg.directoryFossilCollectKind = warpsim::optimistic_migration::DefaultFossilCollectKind;

    warpsim::Simulation sim(cfg, transport);

    auto owner0 = std::make_unique<OwnerLP>(0);
    auto owner1 = std::make_unique<OwnerLP>(1);
    warpsim::DirectoryLPConfig dirCfg;
    dirCfg.id = 2;
    dirCfg.stateEntityId = kDirectoryState;
    dirCfg.updateOwnerKind = DirectoryUpdateOwner;
    dirCfg.decodeUpdate = [](const warpsim::Event &ev) -> warpsim::DirectoryUpdate
    {
        if (ev.payload.bytes.size() != sizeof(UpdateOwnerArgs))
        {
            throw std::runtime_error("DirectoryUpdateOwner: bad payload");
        }
        UpdateOwnerArgs a{};
        std::memcpy(&a, ev.payload.bytes.data(), sizeof(UpdateOwnerArgs));

        warpsim::DirectoryUpdate u;
        u.entity = a.entity;
        u.newOwner = a.newOwner;
        u.sendInstall = true;
        InstallArgs ia{a.entity, a.value};
        u.installPayload.kind = InstallEntityState;
        u.installPayload.bytes = warpsim::bytes_from_trivially_copyable(ia);
        return u;
    };

    dirCfg.fossilCollectKind = warpsim::optimistic_migration::DefaultFossilCollectKind;
    dirCfg.decodeFossilCollect = [](const warpsim::Event &ev) -> warpsim::TimeStamp
    {
        // The kernel encodes GVT as a trivially-copyable TimeStamp.
        if (ev.payload.bytes.size() != sizeof(warpsim::TimeStamp))
        {
            throw std::runtime_error("DirectoryFossilCollect: bad payload");
        }
        warpsim::TimeStamp gvt{};
        std::memcpy(&gvt, ev.payload.bytes.data(), sizeof(warpsim::TimeStamp));
        return gvt;
    };

    auto dir = std::make_unique<warpsim::DirectoryLP>(dirCfg);
    auto driver = std::make_unique<DriverLP>(3, /*directory=*/2);

    auto *owner0p = owner0.get();
    auto *owner1p = owner1.get();

    sim.add_lp(std::move(owner0));
    sim.add_lp(std::move(owner1));
    sim.add_lp(std::move(dir));
    sim.add_lp(std::move(driver));

    // Drive execution forward with local ticks; driver emits the interesting messages.
    for (std::uint64_t t = 1; t <= 10; ++t)
    {
        warpsim::Event tick;
        tick.ts = warpsim::TimeStamp{t, 0};
        tick.src = 3;
        tick.dst = 3;
        tick.payload.kind = DriverTick;
        sim.schedule(std::move(tick));

        // Keep owners moving forward to allow rollbacks to be observable.
        warpsim::Event o0;
        o0.ts = warpsim::TimeStamp{t, 0};
        o0.src = 0;
        o0.dst = 0;
        o0.payload.kind = DriverTick;
        sim.schedule(std::move(o0));

        warpsim::Event o1;
        o1.ts = warpsim::TimeStamp{t, 0};
        o1.src = 1;
        o1.dst = 1;
        o1.payload.kind = DriverTick;
        sim.schedule(std::move(o1));
    }

    sim.run();

    // The straggler migration update at t=5 sets owner to LP1, so the event at t=7
    // must ultimately execute on LP1 (and be cancelled/rolled back on LP0).
    assert(owner0p->use_count() == 0);
    assert(owner1p->use_count() == 1);

    // Migration state must have been installed at LP1.
    assert(owner1p->installed_value() == 99);

    return 0;
}
