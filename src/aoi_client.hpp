#pragma once

#include "aoi_lp.hpp"
#include "common.hpp"
#include "event.hpp"
#include "lp.hpp"

#include <span>

namespace warpsim
{
    // Convenience helpers to use AoiLP without repeating boilerplate.
    struct AoiClient
    {
        LPId aoiLp = 0;
        std::uint32_t updateKind = 0;
        std::uint32_t queryKind = 0;

        void send_update(IEventSink &sink, TimeStamp ts, LPId srcLp, EntityId entity, Vec3 pos) const
        {
            Event ev;
            ev.ts = ts;
            ev.src = srcLp;
            ev.dst = aoiLp;
            ev.target = 0;
            ev.payload.kind = updateKind;
            ev.payload.bytes = bytes_from_trivially_copyable(AoiLP::UpdateArgs{.entity = entity, .pos = pos});
            sink.send(std::move(ev));
        }

        void send_query(IEventSink &sink, TimeStamp ts, LPId srcLp, const AoiLP::QueryArgs &q) const
        {
            Event ev;
            ev.ts = ts;
            ev.src = srcLp;
            ev.dst = aoiLp;
            ev.target = 0;
            ev.payload.kind = queryKind;
            ev.payload.bytes = bytes_from_trivially_copyable(q);
            sink.send(std::move(ev));
        }

        static AoiLP::Result decode_result(const Payload &p)
        {
            return AoiLP::decode_result_bytes(std::span<const std::byte>(p.bytes.data(), p.bytes.size()));
        }
    };
}
