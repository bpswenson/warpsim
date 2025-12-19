#pragma once

#include "common.hpp"
#include "state_store.hpp"
#include "wire.hpp"

namespace warpsim
{
    struct Migration
    {
        EntityId entity = 0;
        LPId newOwner = 0;
        TimeStamp effectiveTs{0, 0};
        ByteBuffer state;
    };

    inline ByteBuffer encode_migration(const Migration &m)
    {
        WireWriter w;
        w.write_u64(m.entity);
        w.write_u32(m.newOwner);
        w.write_u64(m.effectiveTs.time);
        w.write_u64(m.effectiveTs.sequence);
        w.write_bytes(std::span<const std::byte>(m.state.data(), m.state.size()));
        return w.take();
    }

    inline Migration decode_migration(std::span<const std::byte> bytes)
    {
        WireReader r(bytes);
        Migration m;
        m.entity = r.read_u64();
        m.newOwner = r.read_u32();
        m.effectiveTs.time = r.read_u64();
        m.effectiveTs.sequence = r.read_u64();
        m.state = r.read_bytes();
        return m;
    }
}
