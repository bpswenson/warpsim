#pragma once

#include "event.hpp"
#include "wire.hpp"

namespace warpsim
{
    inline ByteBuffer encode_event(const Event &ev)
    {
        WireWriter w;
        w.write_u64(ev.ts.time);
        w.write_u64(ev.ts.sequence);
        w.write_u32(ev.src);
        w.write_u32(ev.dst);
        w.write_u64(ev.target);
        w.write_u64(ev.uid);
        w.write_u8(ev.gvtColor);
        w.write_u8(static_cast<std::uint8_t>(ev.isAnti ? 1 : 0));
        w.write_u32(ev.payload.kind);
        w.write_bytes(std::span<const std::byte>(ev.payload.bytes.data(), ev.payload.bytes.size()));
        return w.take();
    }

    inline Event decode_event(std::span<const std::byte> bytes)
    {
        WireReader r(bytes);
        Event ev;
        ev.ts.time = r.read_u64();
        ev.ts.sequence = r.read_u64();
        ev.src = r.read_u32();
        ev.dst = r.read_u32();
        ev.target = r.read_u64();
        ev.uid = r.read_u64();
        ev.gvtColor = r.read_u8();
        ev.isAnti = (r.read_u8() != 0);
        ev.payload.kind = r.read_u32();
        ev.payload.bytes = r.read_bytes();
        return ev;
    }
}
