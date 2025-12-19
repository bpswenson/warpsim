#pragma once

#include "common.hpp"
#include "state_store.hpp"

namespace warpsim
{
    // Unique id to match anti-messages to the original event.
    using EventUid = std::uint64_t;

    struct Event
    {
        TimeStamp ts{};
        LPId src = 0;
        LPId dst = 0;
        EntityId target = 0;
        EventUid uid = 0;
        Payload payload{};
        // Used by algorithmic GVT (colored messages). 0=white, 1=red.
        std::uint8_t gvtColor = 0;
        bool isAnti = false;
    };

    struct EventTimeOrder
    {
        bool operator()(const Event &a, const Event &b) const noexcept
        {
            if (a.ts < b.ts)
            {
                return true;
            }
            if (b.ts < a.ts)
            {
                return false;
            }
            return a.uid < b.uid;
        }
    };
}
