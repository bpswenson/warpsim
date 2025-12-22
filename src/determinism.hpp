#pragma once

#include "event.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace warpsim
{
    // A commutative, multiplicity-sensitive digest for determinism checks.
    //
    // Goal: allow comparison across different MPI rank counts where the partitioning of LPs
    // across ranks changes. To achieve that, the digest is accumulated as a multiset hash:
    // each committed item contributes a per-item 64-bit hash that is combined via both SUM
    // and XOR. SUM preserves multiplicity; XOR is an additional independent check.
    struct DeterminismDigest
    {
        std::uint64_t committedEventSum = 0;
        std::uint64_t committedEventXor = 0;
        std::uint64_t committedOutputSum = 0;
        std::uint64_t committedOutputXor = 0;

        bool operator==(const DeterminismDigest &o) const noexcept
        {
            return committedEventSum == o.committedEventSum && committedEventXor == o.committedEventXor &&
                   committedOutputSum == o.committedOutputSum && committedOutputXor == o.committedOutputXor;
        }
    };

    namespace detail
    {
        inline std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept
        {
            std::uint64_t h = 1469598103934665603ULL;
            for (const std::byte b : bytes)
            {
                h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(b));
                h *= 1099511628211ULL;
            }
            return h;
        }

        template <class T>
        inline void append_trivial(std::vector<std::byte> &out, const T &v)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const std::size_t off = out.size();
            out.resize(off + sizeof(T));
            std::memcpy(out.data() + off, &v, sizeof(T));
        }

        inline void append_bytes(std::vector<std::byte> &out, std::span<const std::byte> b)
        {
            const std::uint64_t n = static_cast<std::uint64_t>(b.size());
            append_trivial(out, n);
            const std::size_t off = out.size();
            out.resize(off + b.size());
            if (!b.empty())
            {
                std::memcpy(out.data() + off, b.data(), b.size());
            }
        }

        inline std::uint64_t hash_committed_event(LPId lp, const Event &ev)
        {
            // Include stable identity fields + payload.
            // Note: dst/src can change due to directory forwarding, etc. lp is the executor.
            std::vector<std::byte> buf;
            buf.reserve(64 + ev.payload.bytes.size());
            append_trivial(buf, lp);
            append_trivial(buf, ev.ts.time);
            append_trivial(buf, ev.ts.sequence);
            append_trivial(buf, ev.src);
            append_trivial(buf, ev.uid);
            append_trivial(buf, ev.target);
            append_trivial(buf, ev.payload.kind);
            const std::uint8_t anti = ev.isAnti ? 1u : 0u;
            append_trivial(buf, anti);
            append_bytes(buf, std::span<const std::byte>(ev.payload.bytes.data(), ev.payload.bytes.size()));
            return fnv1a64(std::span<const std::byte>(buf.data(), buf.size()));
        }

        inline std::uint64_t hash_committed_output(LPId lp, TimeStamp ts, const Payload &p)
        {
            std::vector<std::byte> buf;
            buf.reserve(48 + p.bytes.size());
            append_trivial(buf, lp);
            append_trivial(buf, ts.time);
            append_trivial(buf, ts.sequence);
            append_trivial(buf, p.kind);
            append_bytes(buf, std::span<const std::byte>(p.bytes.data(), p.bytes.size()));
            return fnv1a64(std::span<const std::byte>(buf.data(), buf.size()));
        }
    }

    class DeterminismAccumulator
    {
    public:
        void on_committed_event(RankId, LPId lp, const Event &ev)
        {
            const std::uint64_t h = detail::hash_committed_event(lp, ev);
            m_digest.committedEventSum += h;
            m_digest.committedEventXor ^= h;
        }

        void on_committed_output(RankId, LPId lp, TimeStamp ts, const Payload &payload)
        {
            const std::uint64_t h = detail::hash_committed_output(lp, ts, payload);
            m_digest.committedOutputSum += h;
            m_digest.committedOutputXor ^= h;
        }

        const DeterminismDigest &digest() const noexcept { return m_digest; }

    private:
        DeterminismDigest m_digest{};
    };
}
