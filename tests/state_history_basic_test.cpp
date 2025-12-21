#include "state_store.hpp"

#include <cassert>
#include <cstddef>
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
    warpsim::StateHistory h(/*maxSnapshots=*/16);

    // Empty history
    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{0, 0});
        assert(!s.has_value());
    }

    h.push(warpsim::TimeStamp{10, 1}, bytes_from_string("a"));
    h.push(warpsim::TimeStamp{10, 5}, bytes_from_string("b"));
    h.push(warpsim::TimeStamp{12, 1}, bytes_from_string("c"));

    // Exact hit
    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{10, 5});
        assert(s.has_value());
        assert((s->first == warpsim::TimeStamp{10, 5}));
        assert(string_from_bytes(s->second) == "b");
    }

    // In-between sequences within same time
    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{10, 3});
        assert(s.has_value());
        assert((s->first == warpsim::TimeStamp{10, 1}));
        assert(string_from_bytes(s->second) == "a");
    }

    // Max-sequence query should pick the last snapshot at that time
    {
        auto s = h.latest_at_or_before(warpsim::stamp_at_max_sequence(10));
        assert(s.has_value());
        assert((s->first == warpsim::TimeStamp{10, 5}));
        assert(string_from_bytes(s->second) == "b");
    }

    // Between times
    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{11, 0});
        assert(s.has_value());
        assert((s->first == warpsim::TimeStamp{10, 5}));
        assert(string_from_bytes(s->second) == "b");
    }

    // Before first
    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{9, 999});
        assert(!s.has_value());
    }

    return 0;
}
