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
    warpsim::StateHistory h(/*maxSnapshots=*/16);

    h.push(warpsim::TimeStamp{10, 1}, bytes_from_string("a"));
    h.push(warpsim::TimeStamp{10, 5}, bytes_from_string("b"));
    h.push(warpsim::TimeStamp{12, 1}, bytes_from_string("c"));

    // Drop everything strictly after (10,5): should drop the (12,1) snapshot.
    h.drop_after(warpsim::TimeStamp{10, 5});

    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{100, 0});
        assert(s.has_value());
        assert((s->first == warpsim::TimeStamp{10, 5}));
        assert(string_from_bytes(s->second) == "b");
    }

    // Drop after (10,1): should drop (10,5) too.
    h.drop_after(warpsim::TimeStamp{10, 1});

    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{11, 0});
        assert(s.has_value());
        assert((s->first == warpsim::TimeStamp{10, 1}));
        assert(string_from_bytes(s->second) == "a");
    }

    // Drop after before first: should clear all.
    h.drop_after(warpsim::TimeStamp{0, 0});
    {
        auto s = h.latest_at_or_before(warpsim::TimeStamp{100, 0});
        assert(!s.has_value());
    }

    return 0;
}
