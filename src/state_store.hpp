#pragma once

#include "common.hpp"

namespace warpsim
{
    struct TimeStamp
    {
        uint64_t time = 0;
        uint64_t sequence = 0;

        friend constexpr bool operator<(const TimeStamp &lhs, const TimeStamp &rhs)
        {
            return (lhs.time < rhs.time) || ((lhs.time == rhs.time) && (lhs.sequence < rhs.sequence));
        }
        friend constexpr bool operator==(const TimeStamp &lhs, const TimeStamp &rhs)
        {
            return (lhs.time == rhs.time) && (lhs.sequence == rhs.sequence);
        }
    };

    inline constexpr TimeStamp stamp_at_max_sequence(std::uint64_t time) noexcept
    {
        return TimeStamp{time, std::numeric_limits<std::uint64_t>::max()};
    }

    class StateHistory
    {
    public:
        explicit StateHistory(std::size_t maxSnapshots = 1024) : m_maxSnapshots(maxSnapshots) {}

        void clear()
        {
            m_history.clear();
        }

        void push(TimeStamp ts, ByteBuffer snapshot)
        {
            m_history.emplace(ts, std::move(snapshot));
            trim_();
        }

        // Returns the latest snapshot at or before `ts`.
        // If none exists, returns nullopt.
        std::optional<std::pair<TimeStamp, std::span<const std::byte>>> latest_at_or_before(TimeStamp ts) const
        {
            if (m_history.empty())
            {
                return std::nullopt;
            }

            auto it = m_history.upper_bound(ts);
            if (it == m_history.begin())
            {
                return std::nullopt;
            }
            --it;
            return std::make_optional(std::pair<TimeStamp, std::span<const std::byte>>{it->first, std::span<const std::byte>(it->second.data(), it->second.size())});
        }

        void drop_after(TimeStamp ts)
        {
            auto it = m_history.upper_bound(ts);
            m_history.erase(it, m_history.end());
        }

    private:
        void trim_()
        {
            while (m_history.size() > m_maxSnapshots)
            {
                m_history.erase(m_history.begin());
            }
        }

        std::size_t m_maxSnapshots = 1024;
        std::map<TimeStamp, ByteBuffer> m_history;
    };
}