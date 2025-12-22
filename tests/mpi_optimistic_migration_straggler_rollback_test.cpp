/*
Purpose: MPI end-to-end test that forces rollback in optimistic migration.

What this tests:
- Targeted events are routed via a directory LP across ranks.
- A late (straggler) update-owner with an earlier timestamp forces rollback in the directory.
- The directory cancels the previously forwarded use (via anti) and re-forwards to the new owner.
- The install event is delivered and applied on the new owner even if it arrives as a straggler.
- Every rank participates (has local work), so the test cannot be “gimped” by running only on rank 0.
*/

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
    constexpr warpsim::EntityId kDirectoryState = 0xD1EC7003ULL;

    // Keep owner LP ids away from special LPs.
    constexpr warpsim::LPId kOwnerBaseLpId = 100;

    // Keep directory LP id fixed and stable across rank counts.
    constexpr warpsim::LPId kDirectoryLpId = 2;

    constexpr std::uint32_t KindDriverTick = 91001;
    constexpr std::uint32_t KindUseEntity = 91002;
    constexpr std::uint32_t KindHoldGvt = 91003;

    struct Installed
    {
        std::uint64_t value = 0;
    };

    class OwnerLP final : public warpsim::ILogicalProcess
    {
    public:
        OwnerLP(warpsim::LPId id, bool holdGvt) : m_id(id), m_holdGvt(holdGvt) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Keep global GVT pinned low long enough for the straggler update-owner
            // to arrive and trigger rollback/anti cancellations (before anything at t=7
            // becomes irrevocable). Only one rank needs to do this.
            if (m_holdGvt)
            {
                for (std::uint64_t i = 0; i < 2000; ++i)
                {
                    warpsim::Event hold;
                    hold.ts = warpsim::TimeStamp{1, 0};
                    hold.src = m_id;
                    hold.dst = m_id;
                    hold.payload.kind = KindHoldGvt;
                    sink.send(std::move(hold));
                }
            }

            // Local work on every rank/LP, so no rank can be idle.
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

            if (ev.payload.kind == KindHoldGvt)
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

                // Note: under optimistic execution, a transient history may briefly
                // process a use before the (straggler) install arrives and triggers
                // rollback. We assert correctness on the final state at test end.
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
        bool m_holdGvt = false;
        std::uint64_t m_installedValue = 0;
        std::uint64_t m_useCount = 0;
    };

    class DriverLP final : public warpsim::ILogicalProcess
    {
    public:
        explicit DriverLP(warpsim::LPId id) : m_id(id) {}

        warpsim::LPId id() const noexcept override { return m_id; }

        void on_start(warpsim::IEventSink &sink) override
        {
            // Drive deterministic control flow on rank0 only.
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

            // 1) At driver time 2, enqueue a targeted use at time 7.
            // The directory will process it before the update arrives.
            if (ev.ts.time == 2)
            {
                warpsim::Event use;
                use.ts = warpsim::TimeStamp{7, 0};
                use.src = m_id;
                use.dst = 0; // ignored once target != 0 and directory routing enabled
                use.target = kEntity;
                use.payload.kind = KindUseEntity;
                ctx.send(std::move(use));
                return;
            }

            // 2) At driver time 8, inject a straggler owner update at timestamp 5.
            // This must force rollback in the directory (and likely in owners).
            if (ev.ts.time == 8)
            {
                Installed st{99};
                const warpsim::ByteBuffer state = warpsim::bytes_from_trivially_copyable(st);

                const warpsim::LPId newOwnerLp = kOwnerBaseLpId + 1;

                warpsim::Event upd;
                upd.ts = warpsim::TimeStamp{5, 0};
                upd.src = m_id;
                upd.dst = kDirectoryLpId;
                upd.target = kEntity;
                upd.payload = warpsim::optimistic_migration::make_update_owner_payload(
                    kEntity,
                    newOwnerLp,
                    std::span<const std::byte>(state.data(), state.size()));
                ctx.send(std::move(upd));
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
        // Owner LP ids are assigned as kOwnerBaseLpId + rank, and must map back
        // to that rank regardless of the modulo mapping used for all other LPs.
        const warpsim::LPId ownerBegin = kOwnerBaseLpId;
        const warpsim::LPId ownerEnd = kOwnerBaseLpId + static_cast<warpsim::LPId>(size);
        if (lp >= ownerBegin && lp < ownerEnd)
        {
            return static_cast<warpsim::RankId>(static_cast<int>(lp - ownerBegin));
        }

        return static_cast<warpsim::RankId>(static_cast<int>(lp % static_cast<warpsim::LPId>(size)));
    };

    cfg.gvtReduceMin = [](warpsim::TimeStamp local) -> warpsim::TimeStamp
    { return warpsim::mpi_allreduce_min_timestamp(MPI_COMM_WORLD, local); };

    cfg.anyRankHasWork = [](bool localHasWork) -> bool
    { return warpsim::mpi_allreduce_any_work(MPI_COMM_WORLD, localHasWork); };

    // Directory routing is required for targeted events.
    cfg.directoryForEntity = [](warpsim::EntityId) -> warpsim::LPId
    { return kDirectoryLpId; };

    // Keep the test focused; directory fossil collection is covered elsewhere.
    cfg.directoryFossilCollectKind = 0;

    warpsim::Simulation sim(cfg, transport);

    // One owner LP per rank, but offset to avoid colliding with directory/driver LPIds.
    const warpsim::LPId myLp = kOwnerBaseLpId + static_cast<warpsim::LPId>(rank);
    const bool holdGvt = (rank == (size - 1));
    auto owner = std::make_unique<OwnerLP>(myLp, holdGvt);
    auto *ownerPtr = owner.get();
    sim.add_lp(std::move(owner));

    // Driver lives on rank0 only.
    const warpsim::LPId driverLpId = static_cast<warpsim::LPId>(size * 4); // always maps to rank0
    if (cfg.lpToRank(driverLpId) == cfg.rank)
    {
        sim.add_lp(std::make_unique<DriverLP>(driverLpId));
    }

    // Directory LP lives only on its owning rank.
    if (cfg.lpToRank(kDirectoryLpId) == cfg.rank)
    {
        auto dirCfg = warpsim::optimistic_migration::make_default_directory_config(kDirectoryLpId, kDirectoryState);
        // Default owner before update: rank0's owner LP.
        dirCfg.defaultOwner = [](warpsim::EntityId, warpsim::TimeStamp) -> warpsim::LPId
        { return kOwnerBaseLpId + 0; };
        sim.add_lp(std::make_unique<warpsim::DirectoryLP>(std::move(dirCfg)));
    }

    sim.run();

    // --- Assertions ---------------------------------------------------------
    // Migration behavior: the use at t=7 must be processed by rank1's owner LP only.
    const warpsim::LPId ownerLp0 = kOwnerBaseLpId + 0;
    const warpsim::LPId ownerLp1 = kOwnerBaseLpId + 1;

    std::uint64_t use0Local = (myLp == ownerLp0) ? ownerPtr->use_count() : 0;
    std::uint64_t use1Local = (myLp == ownerLp1) ? ownerPtr->use_count() : 0;
    std::uint64_t installed1Local = (myLp == ownerLp1) ? ownerPtr->installed_value() : 0;

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
                     "mpi_optimistic_migration_straggler_rollback_test failed: use0=%llu use1=%llu installed1=%llu\n",
                     static_cast<unsigned long long>(use0),
                     static_cast<unsigned long long>(use1),
                     static_cast<unsigned long long>(installed1));
    }

    MPI_Finalize();
    return (okGlobal == 1) ? 0 : 2;
}
