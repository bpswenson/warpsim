/*
Purpose: Negative tests for wire decoding.

What this tests: Decoding functions reject truncated or malformed buffers by throwing,
so bad wire data cannot silently produce corrupted events/migrations.
*/

#include "event_wire.hpp"
#include "migration.hpp"

#include <cassert>
#include <cstddef>
#include <vector>

namespace
{
    template <typename Fn>
    void expect_throw(Fn &&fn)
    {
        bool threw = false;
        try
        {
            fn();
        }
        catch (...)
        {
            threw = true;
        }
        assert(threw);
    }
}

int main()
{
    // Event: empty buffer is truncated.
    expect_throw([]
                 { (void)warpsim::decode_event({}); });

    // Event: too-short fixed header is truncated.
    {
        std::vector<std::byte> b(8); // needs much more than 8 bytes
        expect_throw([&]
                     { (void)warpsim::decode_event(std::span<const std::byte>(b.data(), b.size())); });
    }

    // Migration: empty buffer is truncated.
    expect_throw([]
                 { (void)warpsim::decode_migration({}); });

    // Migration: header present but claims more state bytes than exist.
    {
        warpsim::Migration m;
        m.entity = 1;
        m.newOwner = 2;
        m.effectiveTs = warpsim::TimeStamp{3, 4};
        m.state = {std::byte{0xAA}};

        warpsim::ByteBuffer bytes = warpsim::encode_migration(m);
        bytes.resize(bytes.size() - 1); // truncate

        expect_throw([&]
                     { (void)warpsim::decode_migration(std::span<const std::byte>(bytes.data(), bytes.size())); });
    }

    return 0;
}
