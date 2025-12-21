#include "directory_lp.hpp"
#include "mpi_collectives.hpp"
#include "mpi_transport.hpp"
#include "optimistic_migration.hpp"
#include "simulation.hpp"

#include <mpi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
    constexpr warpsim::EntityId kEntity = 0x1111222233334444ULL;
    constexpr warpsim::EntityId kOwnerState = 0x0ABCDEF0ULL;
    constexpr warpsim::EntityId kDirectoryState = 0xD1EC7000ULL;

    constexpr warpsim::LPId kDirectoryLpId = 2; // maps to rank 0 when size==2

    constexpr std::uint32_t KindDriverTick = 7001;
    constexpr std::uint32_t KindUseEntity = 7002;

    struct Installed
    {
        std::uint64_t value = 0;
    };

    class OwnerLP final : public warpsim::ILogicalProcess
    {
    public:
        OwnerLP(warpsim::LPId id, int rank, int size)
            : m_id(id), m_rank(rank), m_size(size)
        {
        }

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Drive progress everywhere so GVT can advance and rollbacks are observable.
            for (std::uint64_t t = 1; t <= 12; ++t)
            {
                warpsim::Event tick;
                tick.ts = warpsim::TimeStamp{t, 0};
                tick.src = m_id;
                tick.dst = m_id;
                tick.payload.kind = KindDriverTick;
                sink.send(std::move(tick));
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind == KindDriverTick)
            {
                return;
            }

            if (ev.payload.kind == warpsim::optimistic_migration::DefaultInstallKind)
            {
                const auto [entity, bytes] = warpsim::optimistic_migration::decode_install(ev);
                if (entity != kEntity)
                {
                    return;
                }

                ctx.request_write(kOwnerState);
                Installed st{};
                if (bytes.size() == sizeof(Installed))
                {
                    std::memcpy(&st, bytes.data(), sizeof(Installed));
                }
                m_installedValue = st.value;
                return;
            }

            if (ev.payload.kind == KindUseEntity)
            {
                if (ev.target != kEntity)
                {
                    return;
                }

                ctx.request_write(kOwnerState);
                ++m_useCount;
                return;
            }
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
                return;
            }
            S s{};
            std::memcpy(&s, state.data(), sizeof(S));
            m_installedValue = s.installedValue;
            m_useCount = s.useCount;
        }

        std::uint64_t use_count() const { return m_useCount; }
        std::uint64_t installed_value() const { return m_installedValue; }

    private:
        warpsim::LPId m_id = 0;
        int m_rank = 0;
        int m_size = 1;
        std::uint64_t m_installedValue = 0;
        std::uint64_t m_useCount = 0;
    };

    class DriverLP final : public warpsim::ILogicalProcess
    {
    public:
        DriverLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Driver lives on rank0 only; it deterministically sends the two key messages.
            for (std::uint64_t t = 1; t <= 12; ++t)
            {
                warpsim::Event tick;
                tick.ts = warpsim::TimeStamp{t, 0};
                tick.src = m_id;
                tick.dst = m_id;
                tick.payload.kind = KindDriverTick;
                sink.send(std::move(tick));
            }
        }

        void on_event(const warpsim::Event &ev, warpsim::IEventContext &ctx) override
        {
            if (ev.payload.kind != KindDriverTick)
            {
                return;
            }

            // First establish ownership at timestamp 5, then issue a use at timestamp 7.
            // This exercises cross-rank directory routing + install without relying on
            // rollback/anti behavior in MPI.

            if (ev.ts.time == 2)
            {
                Installed st{99};
                const warpsim::ByteBuffer state = warpsim::bytes_from_trivially_copyable(st);

                warpsim::Event upd;
                upd.ts = warpsim::TimeStamp{5, 0};
                upd.src = m_id;
                upd.dst = kDirectoryLpId;
                upd.target = kEntity;
                upd.payload = warpsim::optimistic_migration::make_update_owner_payload(
                    kEntity,
                    /*newOwner=*/1,
                    std::span<const std::byte>(state.data(), state.size()));
                ctx.send(std::move(upd));
                return;
            }

            // At logical time 6: send a targeted use at time 7.
            if (ev.ts.time == 6)
            {
                warpsim::Event use;
                use.ts = warpsim::TimeStamp{7, 0};
                use.src = m_id;
                use.dst = 0; // ignored when target != 0 and directory routing enabled
                use.target = kEntity;
                use.payload.kind = KindUseEntity;
                ctx.send(std::move(use));
                return;
            }
        }

        warpsim::ByteBuffer save_state() const override { return {}; }
        void load_state(std::span<const std::byte>) override {}

    private:
        warpsim::LPId m_id = 0;
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

    auto transport = std::make_shared<warpsim::MpiTransport>(MPI_COMM_WORLD);

    warpsim::SimulationConfig cfg;
    cfg.rank = static_cast<warpsim::RankId>(rank);
    cfg.lpToRank = [size](warpsim::LPId lp) -> warpsim::RankId
    {
        return static_cast<warpsim::RankId>(static_cast<int>(lp % static_cast<warpsim::LPId>(size)));
    };

    cfg.gvtReduceMin = [](warpsim::TimeStamp local) -> warpsim::TimeStamp
    { return warpsim::mpi_allreduce_min_timestamp(MPI_COMM_WORLD, local); };

    cfg.anyRankHasWork = [](bool localHasWork) -> bool
    { return warpsim::mpi_allreduce_any_work(MPI_COMM_WORLD, localHasWork); };

    // Directory routing is the multi-rank mechanism: all targeted events go via the directory.
    cfg.directoryForEntity = [](warpsim::EntityId) -> warpsim::LPId
    { return kDirectoryLpId; };

    // Keep this test focused: directory fossil-collection notifications are covered elsewhere.
    cfg.directoryFossilCollectKind = 0;

    warpsim::Simulation sim(cfg, transport);

    // One owner LP per rank.
    const warpsim::LPId myLp = static_cast<warpsim::LPId>(rank);
    auto owner = std::make_unique<OwnerLP>(myLp, rank, size);
    auto *ownerPtr = owner.get();
    sim.add_lp(std::move(owner));

    // Dedicated driver on rank0 so rollbacks on owner0 can't re-send updates.
    const warpsim::LPId driverLpId = static_cast<warpsim::LPId>(size * 4); // always maps to rank0
    if (cfg.lpToRank(driverLpId) == cfg.rank)
    {
        sim.add_lp(std::make_unique<DriverLP>(driverLpId));
    }

    // Directory LP lives only on its owning rank.
    if (cfg.lpToRank(kDirectoryLpId) == cfg.rank)
    {
        warpsim::DirectoryLPConfig dirCfg = warpsim::optimistic_migration::make_default_directory_config(
            kDirectoryLpId, kDirectoryState);
        dirCfg.defaultOwner = [](warpsim::EntityId, warpsim::TimeStamp) -> warpsim::LPId
        { return 0; };
        sim.add_lp(std::make_unique<warpsim::DirectoryLP>(std::move(dirCfg)));
    }

    sim.run();

    // Collect global assertions.
    std::uint64_t use0Local = 0;
    std::uint64_t use1Local = 0;
    std::uint64_t installed1Local = 0;

    if (myLp == 0)
    {
        use0Local = ownerPtr->use_count();
    }
    if (myLp == 1)
    {
        use1Local = ownerPtr->use_count();
        installed1Local = ownerPtr->installed_value();
    }

    std::uint64_t use0 = 0;
    std::uint64_t use1 = 0;
    std::uint64_t installed1 = 0;

    MPI_Allreduce(&use0Local, &use0, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&use1Local, &use1, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&installed1Local, &installed1, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    int okLocal = 1;
    okLocal &= (use0 == 0) ? 1 : 0;
    okLocal &= (use1 == 1) ? 1 : 0;
    okLocal &= (installed1 == 99) ? 1 : 0;

    int okGlobal = 0;
    MPI_Allreduce(&okLocal, &okGlobal, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);

    if (rank == 0 && okGlobal != 1)
    {
        std::fprintf(stderr,
                     "mpi_optimistic_migration_directory_test failed: use0=%llu use1=%llu installed1=%llu\n",
                     static_cast<unsigned long long>(use0),
                     static_cast<unsigned long long>(use1),
                     static_cast<unsigned long long>(installed1));
    }

    MPI_Finalize();
    return (okGlobal == 1) ? 0 : 2;
}
