#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace warpsim
{
    using SimTime = std::uint64_t;

    using RankId = std::uint32_t;
    using LPId = std::uint32_t;
    using EntityId = std::uint64_t;

    using ByteBuffer = std::vector<std::byte>;

    struct Payload
    {
        std::uint32_t kind = 0;
        ByteBuffer bytes;
    };

    template <class T>
    inline ByteBuffer bytes_from_trivially_copyable(const T &value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        ByteBuffer out(sizeof(T));
        std::memcpy(out.data(), &value, sizeof(T));
        return out;
    }
}