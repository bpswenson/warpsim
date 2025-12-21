#include "migration.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace
{
    warpsim::ByteBuffer bytes_from_string(std::string_view s)
    {
        warpsim::ByteBuffer out;
        out.resize(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            out[i] = static_cast<std::byte>(s[i]);
        }
        return out;
    }
}

int main()
{
    warpsim::Migration m;
    m.entity = 0x12345678ULL;
    m.newOwner = 42;
    m.effectiveTs = warpsim::TimeStamp{100, 7};
    m.state = bytes_from_string("hello migration");

    const warpsim::ByteBuffer bytes = warpsim::encode_migration(m);
    const warpsim::Migration d = warpsim::decode_migration(std::span<const std::byte>(bytes.data(), bytes.size()));

    assert(d.entity == m.entity);
    assert(d.newOwner == m.newOwner);
    assert(d.effectiveTs.time == m.effectiveTs.time);
    assert(d.effectiveTs.sequence == m.effectiveTs.sequence);
    assert(d.state.size() == m.state.size());
    assert(d.state == m.state);

    return 0;
}
