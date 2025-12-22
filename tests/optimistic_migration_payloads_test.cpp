/*
Purpose: Unit tests for optimistic migration payload helpers.

What this tests: Helper functions correctly build and parse update-owner, directory-update,
and install payloads (including basic validation/error paths).
*/

#include "optimistic_migration.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace
{
    warpsim::ByteBuffer bytes_from_string(std::string_view s)
    {
        warpsim::ByteBuffer out;
        out.resize(s.size());
        if (!s.empty())
        {
            std::memcpy(out.data(), s.data(), s.size());
        }
        return out;
    }

    void expect_throw(bool didThrow)
    {
        assert(didThrow);
    }
}

int main()
{
    constexpr warpsim::EntityId kEntity = 0x1111222233334444ULL;

    // Update-owner -> DirectoryUpdate -> Install roundtrip.
    {
        const warpsim::ByteBuffer state = bytes_from_string("state-bytes");
        const warpsim::Payload p = warpsim::optimistic_migration::make_update_owner_payload(
            kEntity, /*newOwner=*/7, std::span<const std::byte>(state.data(), state.size()));

        warpsim::Event ev;
        ev.ts = warpsim::TimeStamp{10, 1};
        ev.src = 1;
        ev.dst = 2;
        ev.target = kEntity;
        ev.payload = p;

        const warpsim::DirectoryUpdate u = warpsim::optimistic_migration::decode_update_owner_as_directory_update(ev);
        assert(u.entity == kEntity);
        assert(u.newOwner == 7);
        assert(u.sendInstall);
        assert(u.installPayload.kind == warpsim::optimistic_migration::DefaultInstallKind);

        warpsim::Event install;
        install.ts = ev.ts;
        install.src = 2;
        install.dst = 7;
        install.target = kEntity;
        install.payload = u.installPayload;

        const auto [entity, bytes] = warpsim::optimistic_migration::decode_install(install);
        assert(entity == kEntity);
        assert(bytes.size() == state.size());
        assert(std::memcmp(bytes.data(), state.data(), state.size()) == 0);
    }

    // Fossil-collect payload roundtrip.
    {
        const warpsim::TimeStamp gvt{123, 999};
        warpsim::Event ev;
        ev.payload = warpsim::optimistic_migration::make_fossil_collect_payload(gvt);
        const warpsim::TimeStamp d = warpsim::optimistic_migration::decode_fossil_collect_payload(ev);
        assert(d.time == gvt.time);
        assert(d.sequence == gvt.sequence);
    }

    // Error paths: wrong kinds.
    {
        bool threw = false;
        try
        {
            warpsim::Event ev;
            ev.payload.kind = 999;
            (void)warpsim::optimistic_migration::decode_fossil_collect_payload(ev);
        }
        catch (const std::exception &)
        {
            threw = true;
        }
        expect_throw(threw);
    }

    {
        bool threw = false;
        try
        {
            warpsim::Event ev;
            ev.payload.kind = warpsim::optimistic_migration::DefaultUpdateOwnerKind;
            ev.payload.bytes.resize(1); // too small
            (void)warpsim::optimistic_migration::decode_update_owner_as_directory_update(ev);
        }
        catch (const std::exception &)
        {
            threw = true;
        }
        expect_throw(threw);
    }

    {
        bool threw = false;
        try
        {
            warpsim::Event ev;
            ev.payload.kind = warpsim::optimistic_migration::DefaultInstallKind;
            ev.payload.bytes.resize(1); // too small
            (void)warpsim::optimistic_migration::decode_install(ev);
        }
        catch (const std::exception &)
        {
            threw = true;
        }
        expect_throw(threw);
    }

    return 0;
}
