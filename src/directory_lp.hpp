#pragma once

#include "lp.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <utility>

namespace warpsim
{
    // Optional committed-output record emitted by DirectoryLP when a migration update is applied.
    // This is rollback-safe (tied to the update-owner event) and intended for verification.
    struct DirectoryMigrationLog
    {
        EntityId entity = 0;
        LPId oldOwner = 0;
        LPId newOwner = 0;
        std::uint8_t sentInstall = 0;
        std::uint8_t _pad[3]{};
    };

    struct DirectoryUpdate
    {
        EntityId entity = 0;
        LPId newOwner = 0;

        // Optional: if `sendInstall` is true, the directory emits an install event
        // to `newOwner` at the cutover timestamp.
        bool sendInstall = false;
        Payload installPayload{};
    };

    struct DirectoryLPConfig
    {
        LPId id = 0;

        // Used with `request_write()` so Time Warp snapshots the directory state
        // before mutating ownership history.
        EntityId stateEntityId = 0;

        // If set, targeted events are treated as ownership updates when
        // `ev.payload.kind == updateOwnerKind`.
        std::uint32_t updateOwnerKind = 0;
        std::function<DirectoryUpdate(const Event &)> decodeUpdate;

        // Optional: if set (non-zero), emit committed output whenever an update-owner event
        // is applied. The payload bytes are a trivially-copyable DirectoryMigrationLog.
        // This makes it easy to verify that migrations occurred (and when) without
        // relying on non-rollback-safe printing.
        std::uint32_t migrationLogKind = 0;

        // Optional: fossil collection for directory ownership history.
        // If set, the directory treats events with `payload.kind == fossilCollectKind`
        // as a request to prune history at/before a provided GVT.
        std::uint32_t fossilCollectKind = 0;
        std::function<TimeStamp(const Event &)> decodeFossilCollect;

        // Optional default owner when there is no history for a given (entity, ts),
        // or when ts is before the first ownership entry.
        std::function<LPId(EntityId, TimeStamp)> defaultOwner;
    };

    // Directory LP for fully optimistic migration experiments.
    //
    // - Simulation::send routes targeted events (`ev.target != 0`) to the directory LP.
    // - The directory forwards to the owner-at-time based on a rollback-safe timeline.
    // - Ownership updates are represented as timestamped events processed by this LP.
    class DirectoryLP final : public ILogicalProcess
    {
    public:
        explicit DirectoryLP(DirectoryLPConfig cfg) : m_cfg(std::move(cfg))
        {
            if (m_cfg.id == 0)
            {
                throw std::runtime_error("DirectoryLP: id must be non-zero");
            }
        }

        LPId id() const noexcept override { return m_cfg.id; }

        void on_event(const Event &ev, IEventContext &ctx) override
        {
            if (m_cfg.fossilCollectKind != 0 && ev.payload.kind == m_cfg.fossilCollectKind)
            {
                if (!m_cfg.decodeFossilCollect)
                {
                    throw std::runtime_error("DirectoryLP: fossilCollectKind set but decodeFossilCollect missing");
                }
                if (m_cfg.stateEntityId == 0)
                {
                    throw std::runtime_error("DirectoryLP: stateEntityId required for fossil collection");
                }

                const TimeStamp gvt = m_cfg.decodeFossilCollect(ev);
                ctx.request_write(m_cfg.stateEntityId);
                prune_history_(gvt);
                return;
            }

            if (m_cfg.updateOwnerKind != 0 && ev.payload.kind == m_cfg.updateOwnerKind)
            {
                if (!m_cfg.decodeUpdate)
                {
                    throw std::runtime_error("DirectoryLP: updateOwnerKind set but decodeUpdate missing");
                }
                if (m_cfg.stateEntityId == 0)
                {
                    throw std::runtime_error("DirectoryLP: stateEntityId required for updates");
                }

                const DirectoryUpdate u = m_cfg.decodeUpdate(ev);

                const LPId oldOwner = owner_at_(u.entity, ev.ts);

                ctx.request_write(m_cfg.stateEntityId);
                m_ownerHistory[u.entity][ev.ts] = u.newOwner;

                if (u.sendInstall)
                {
                    Event install;
                    install.ts = ev.ts;
                    install.src = m_cfg.id;
                    install.dst = u.newOwner;
                    install.target = u.entity;
                    install.payload = u.installPayload;
                    ctx.send(std::move(install));
                }

                if (m_cfg.migrationLogKind != 0)
                {
                    Payload p;
                    p.kind = m_cfg.migrationLogKind;
                    p.bytes = bytes_from_trivially_copyable(DirectoryMigrationLog{
                        .entity = u.entity,
                        .oldOwner = oldOwner,
                        .newOwner = u.newOwner,
                        .sentInstall = static_cast<std::uint8_t>(u.sendInstall ? 1 : 0),
                    });
                    ctx.emit_committed(ev.ts, std::move(p));
                }
                return;
            }

            if (ev.target == 0)
            {
                throw std::runtime_error("DirectoryLP: unexpected non-target event");
            }

            const LPId owner = owner_at_(ev.target, ev.ts);

            Event fwd = ev;
            fwd.src = m_cfg.id;
            fwd.dst = owner;
            // Important: forwarded events must not reuse the original sender's uid,
            // otherwise an anti-message for the forward can tombstone-cancel the original.
            fwd.uid = 0;
            fwd.ts.sequence = 0;
            ctx.send(std::move(fwd));
        }

        ByteBuffer save_state() const override
        {
            // Serialize owner history (small; this is intended for experiments).
            // Format: u32 nEntities, then for each: u64 entity, u32 nEntries,
            // then entries {u64 time,u64 seq,u32 owner}.
            ByteBuffer out;
            auto append = [&](const void *p, std::size_t n)
            {
                const auto *b = static_cast<const std::byte *>(p);
                out.insert(out.end(), b, b + n);
            };

            const std::uint32_t nEntities = static_cast<std::uint32_t>(m_ownerHistory.size());
            append(&nEntities, sizeof(nEntities));

            for (const auto &[entity, timeline] : m_ownerHistory)
            {
                append(&entity, sizeof(entity));
                const std::uint32_t n = static_cast<std::uint32_t>(timeline.size());
                append(&n, sizeof(n));
                for (const auto &[ts, owner] : timeline)
                {
                    append(&ts.time, sizeof(ts.time));
                    append(&ts.sequence, sizeof(ts.sequence));
                    append(&owner, sizeof(owner));
                }
            }
            return out;
        }

        void load_state(std::span<const std::byte> state) override
        {
            auto read = [&](std::size_t &off, void *dst, std::size_t n)
            {
                if (off + n > state.size())
                {
                    throw std::runtime_error("DirectoryLP: bad state");
                }
                std::memcpy(dst, state.data() + off, n);
                off += n;
            };

            std::size_t off = 0;
            std::uint32_t nEntities = 0;
            read(off, &nEntities, sizeof(nEntities));

            m_ownerHistory.clear();
            for (std::uint32_t i = 0; i < nEntities; ++i)
            {
                EntityId entity = 0;
                read(off, &entity, sizeof(entity));
                std::uint32_t n = 0;
                read(off, &n, sizeof(n));
                auto &timeline = m_ownerHistory[entity];
                for (std::uint32_t j = 0; j < n; ++j)
                {
                    TimeStamp ts{};
                    LPId owner = 0;
                    read(off, &ts.time, sizeof(ts.time));
                    read(off, &ts.sequence, sizeof(ts.sequence));
                    read(off, &owner, sizeof(owner));
                    timeline.emplace(ts, owner);
                }
            }
        }

    private:
        void prune_history_(TimeStamp gvt)
        {
            // Keep at most one "floor" entry before GVT, so owner_at_ remains correct
            // for timestamps >= GVT.
            for (auto itEntity = m_ownerHistory.begin(); itEntity != m_ownerHistory.end();)
            {
                auto &timeline = itEntity->second;
                if (timeline.empty())
                {
                    itEntity = m_ownerHistory.erase(itEntity);
                    continue;
                }

                const auto it = timeline.lower_bound(gvt);
                if (it == timeline.begin())
                {
                    // No entries before GVT.
                    ++itEntity;
                    continue;
                }

                if (it != timeline.end() && it->first == gvt)
                {
                    // Entry at GVT exists; safe to drop everything before it.
                    timeline.erase(timeline.begin(), it);
                }
                else
                {
                    // Keep the last entry strictly before GVT.
                    auto keep = std::prev(it);
                    timeline.erase(timeline.begin(), keep);
                }

                if (timeline.empty())
                {
                    itEntity = m_ownerHistory.erase(itEntity);
                }
                else
                {
                    ++itEntity;
                }
            }
        }

        LPId owner_at_(EntityId entity, TimeStamp ts) const
        {
            const auto eIt = m_ownerHistory.find(entity);
            if (eIt == m_ownerHistory.end() || eIt->second.empty())
            {
                return m_cfg.defaultOwner ? m_cfg.defaultOwner(entity, ts) : 0;
            }
            const auto &timeline = eIt->second;
            auto it = timeline.upper_bound(ts);
            if (it == timeline.begin())
            {
                return m_cfg.defaultOwner ? m_cfg.defaultOwner(entity, ts) : 0;
            }
            --it;
            return it->second;
        }

        DirectoryLPConfig m_cfg;
        std::map<EntityId, std::map<TimeStamp, LPId>> m_ownerHistory;
    };
}
