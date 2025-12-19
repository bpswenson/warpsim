#pragma once

#include "directory_lp.hpp"
#include "event.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace warpsim::optimistic_migration
{
    // Default event kinds for the optimistic migration directory protocol.
    // These are only conventions; applications may choose their own kinds.
    inline constexpr std::uint32_t DefaultUpdateOwnerKind = 42001;
    inline constexpr std::uint32_t DefaultInstallKind = 42002;
    inline constexpr std::uint32_t DefaultFossilCollectKind = 42003;

    struct UpdateOwnerHeader
    {
        EntityId entity = 0;
        LPId newOwner = 0;
        std::uint32_t stateBytes = 0;
    };

    struct InstallHeader
    {
        EntityId entity = 0;
        std::uint32_t stateBytes = 0;
    };

    inline Payload make_update_owner_payload(EntityId entity, LPId newOwner, std::span<const std::byte> state)
    {
        if (state.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            throw std::runtime_error("optimistic_migration: state too large");
        }

        UpdateOwnerHeader h;
        h.entity = entity;
        h.newOwner = newOwner;
        h.stateBytes = static_cast<std::uint32_t>(state.size());

        Payload p;
        p.kind = DefaultUpdateOwnerKind;
        p.bytes.resize(sizeof(UpdateOwnerHeader) + state.size());
        std::memcpy(p.bytes.data(), &h, sizeof(UpdateOwnerHeader));
        if (!state.empty())
        {
            std::memcpy(p.bytes.data() + sizeof(UpdateOwnerHeader), state.data(), state.size());
        }
        return p;
    }

    inline DirectoryUpdate decode_update_owner_as_directory_update(const Event &ev)
    {
        if (ev.payload.kind != DefaultUpdateOwnerKind)
        {
            throw std::runtime_error("optimistic_migration: wrong update kind");
        }
        if (ev.payload.bytes.size() < sizeof(UpdateOwnerHeader))
        {
            throw std::runtime_error("optimistic_migration: bad update payload");
        }

        UpdateOwnerHeader h{};
        std::memcpy(&h, ev.payload.bytes.data(), sizeof(UpdateOwnerHeader));
        const std::size_t expected = sizeof(UpdateOwnerHeader) + static_cast<std::size_t>(h.stateBytes);
        if (ev.payload.bytes.size() != expected)
        {
            throw std::runtime_error("optimistic_migration: bad update payload size");
        }

        DirectoryUpdate u;
        u.entity = h.entity;
        u.newOwner = h.newOwner;
        u.sendInstall = true;

        // Install payload is a fixed header + raw state bytes.
        InstallHeader ih;
        ih.entity = h.entity;
        ih.stateBytes = h.stateBytes;

        u.installPayload.kind = DefaultInstallKind;
        u.installPayload.bytes.resize(sizeof(InstallHeader) + static_cast<std::size_t>(h.stateBytes));
        std::memcpy(u.installPayload.bytes.data(), &ih, sizeof(InstallHeader));
        if (h.stateBytes != 0)
        {
            std::memcpy(u.installPayload.bytes.data() + sizeof(InstallHeader),
                        ev.payload.bytes.data() + sizeof(UpdateOwnerHeader),
                        static_cast<std::size_t>(h.stateBytes));
        }

        return u;
    }

    inline std::pair<EntityId, std::span<const std::byte>> decode_install(const Event &ev)
    {
        if (ev.payload.kind != DefaultInstallKind)
        {
            throw std::runtime_error("optimistic_migration: wrong install kind");
        }
        if (ev.payload.bytes.size() < sizeof(InstallHeader))
        {
            throw std::runtime_error("optimistic_migration: bad install payload");
        }

        InstallHeader h{};
        std::memcpy(&h, ev.payload.bytes.data(), sizeof(InstallHeader));
        const std::size_t expected = sizeof(InstallHeader) + static_cast<std::size_t>(h.stateBytes);
        if (ev.payload.bytes.size() != expected)
        {
            throw std::runtime_error("optimistic_migration: bad install payload size");
        }

        const auto *p = ev.payload.bytes.data() + sizeof(InstallHeader);
        return {h.entity, std::span<const std::byte>(p, static_cast<std::size_t>(h.stateBytes))};
    }

    inline Payload make_fossil_collect_payload(TimeStamp gvt)
    {
        Payload p;
        p.kind = DefaultFossilCollectKind;
        p.bytes = bytes_from_trivially_copyable(gvt);
        return p;
    }

    inline TimeStamp decode_fossil_collect_payload(const Event &ev)
    {
        if (ev.payload.kind != DefaultFossilCollectKind)
        {
            throw std::runtime_error("optimistic_migration: wrong fossil-collect kind");
        }
        if (ev.payload.bytes.size() != sizeof(TimeStamp))
        {
            throw std::runtime_error("optimistic_migration: bad fossil-collect payload");
        }
        TimeStamp gvt{};
        std::memcpy(&gvt, ev.payload.bytes.data(), sizeof(TimeStamp));
        return gvt;
    }

    inline DirectoryLPConfig make_default_directory_config(LPId id, EntityId stateEntityId)
    {
        DirectoryLPConfig cfg;
        cfg.id = id;
        cfg.stateEntityId = stateEntityId;
        cfg.updateOwnerKind = DefaultUpdateOwnerKind;
        cfg.decodeUpdate = [](const Event &ev) -> DirectoryUpdate
        { return decode_update_owner_as_directory_update(ev); };
        cfg.fossilCollectKind = DefaultFossilCollectKind;
        cfg.decodeFossilCollect = [](const Event &ev) -> TimeStamp
        { return decode_fossil_collect_payload(ev); };
        return cfg;
    }
}
