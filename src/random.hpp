#pragma once

#include "event.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace warpsim
{
    // Stateless deterministic RNG for Time Warp.
    //
    // Design goals:
    // - Deterministic across rollback and MPI rank remaps (uses LPId, not rank).
    // - No mutable state required in LPs (pure function of stable identifiers).
    // - Fast and dependency-free.

    inline std::uint64_t splitmix64(std::uint64_t x) noexcept
    {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    inline std::uint64_t mix_u64(std::uint64_t a, std::uint64_t b) noexcept
    {
        return splitmix64(a ^ splitmix64(b));
    }

    // Return a deterministic 64-bit value for a particular "draw".
    //
    // Parameters:
    // - seed: global simulation seed
    // - lp: logical process id (stable across rank mappings)
    // - stream: user-chosen stream id to separate subsystems
    // - eventUid: 64-bit key for the event being processed
    // - draw: draw index within that event (0,1,2...)
    //
    // Note: EventUid is not guaranteed globally unique across all senders; if you are
    // hashing actual events, prefer the overload that also includes eventSrc.
    inline std::uint64_t rng_u64(std::uint64_t seed,
                                 LPId lp,
                                 std::uint32_t stream,
                                 EventUid eventUid,
                                 std::uint32_t draw = 0) noexcept
    {
        std::uint64_t x = seed;
        x = mix_u64(x, static_cast<std::uint64_t>(lp));
        x = mix_u64(x, static_cast<std::uint64_t>(stream));
        x = mix_u64(x, static_cast<std::uint64_t>(eventUid));
        x = mix_u64(x, static_cast<std::uint64_t>(draw));
        return splitmix64(x);
    }

    // Overload that additionally salts by the event sender (src). This is useful when
    // EventUid is only unique per-src.
    inline std::uint64_t rng_u64(std::uint64_t seed,
                                 LPId lp,
                                 std::uint32_t stream,
                                 LPId eventSrc,
                                 EventUid eventUid,
                                 std::uint32_t draw = 0) noexcept
    {
        std::uint64_t x = seed;
        x = mix_u64(x, static_cast<std::uint64_t>(lp));
        x = mix_u64(x, static_cast<std::uint64_t>(stream));
        x = mix_u64(x, static_cast<std::uint64_t>(eventSrc));
        x = mix_u64(x, static_cast<std::uint64_t>(eventUid));
        x = mix_u64(x, static_cast<std::uint64_t>(draw));
        return splitmix64(x);
    }

    // Uniform in [0,1).
    inline double rng_unit_double(std::uint64_t seed,
                                  LPId lp,
                                  std::uint32_t stream,
                                  EventUid eventUid,
                                  std::uint32_t draw = 0) noexcept
    {
        // Use the top 53 bits to construct a double in [0,1).
        const std::uint64_t r = rng_u64(seed, lp, stream, eventUid, draw);
        const std::uint64_t mantissa = r >> 11;                            // 53 bits
        return static_cast<double>(mantissa) * (1.0 / 9007199254740992.0); // 2^53
    }

    inline double rng_unit_double(std::uint64_t seed,
                                  LPId lp,
                                  std::uint32_t stream,
                                  LPId eventSrc,
                                  EventUid eventUid,
                                  std::uint32_t draw = 0) noexcept
    {
        const std::uint64_t r = rng_u64(seed, lp, stream, eventSrc, eventUid, draw);
        const std::uint64_t mantissa = r >> 11;
        return static_cast<double>(mantissa) * (1.0 / 9007199254740992.0);
    }

    // Uniform integer in [lo, hi] (inclusive). Undefined if lo > hi.
    inline std::uint64_t rng_u64_range(std::uint64_t seed,
                                       LPId lp,
                                       std::uint32_t stream,
                                       EventUid eventUid,
                                       std::uint64_t lo,
                                       std::uint64_t hi,
                                       std::uint32_t draw = 0) noexcept
    {
        const std::uint64_t span = (hi - lo) + 1;
        const std::uint64_t r = rng_u64(seed, lp, stream, eventUid, draw);
        // Modulo bias is acceptable for most simulation use; can upgrade later if needed.
        return lo + (r % span);
    }

    inline std::uint64_t rng_u64_range(std::uint64_t seed,
                                       LPId lp,
                                       std::uint32_t stream,
                                       LPId eventSrc,
                                       EventUid eventUid,
                                       std::uint64_t lo,
                                       std::uint64_t hi,
                                       std::uint32_t draw = 0) noexcept
    {
        const std::uint64_t span = (hi - lo) + 1;
        const std::uint64_t r = rng_u64(seed, lp, stream, eventSrc, eventUid, draw);
        return lo + (r % span);
    }
}
