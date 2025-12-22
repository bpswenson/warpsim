/*
Purpose: Tests StateHistory capacity trimming.

What this tests: When maxSnapshots is exceeded, the oldest snapshots are trimmed and
later queries still return correct data from the remaining snapshots.
*/

#include "state_store.hpp"

#include <cassert>
#include <cstring>
#include <string>
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

    std::string string_from_bytes(std::span<const std::byte> b)
    {
        return std::string(reinterpret_cast<const char *>(b.data()), b.size());
    }
}

int main()
{
    warpsim::StateHistory h(/*maxSnapshots=*/2);

    h.push(warpsim::TimeStamp{1, 1}, bytes_from_string("a"));
    h.push(warpsim::TimeStamp{2, 1}, bytes_from_string("b"));
    h.push(warpsim::TimeStamp{3, 1}, bytes_from_string("c"));

    // Oldest should have been trimmed (1,1).
    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{1, 1});
        assert(!s.has_value());
    }

    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{2, 1});
        assert(s.has_value());
        assert((s->first == warpsim::TimeStamp{2, 1}));
        assert(string_from_bytes(s->second) == "b");
    }

    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{3, 1});
        assert(s.has_value());
        assert((s->first == warpsim::TimeStamp{3, 1}));
        assert(string_from_bytes(s->second) == "c");
    }

    return 0;
}
