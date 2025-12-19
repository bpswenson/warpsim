#pragma once

#include "common.hpp"

#include <deque>
#include <mutex>

namespace warpsim
{
    enum class MessageKind : std::uint8_t
    {
        Event = 1,
        Migration = 2,
        Ack = 3,
    };

    struct WireMessage
    {
        MessageKind kind = MessageKind::Event;
        RankId srcRank = 0;
        RankId dstRank = 0;
        ByteBuffer bytes;
    };

    class ITransport
    {
    public:
        virtual ~ITransport() = default;
        virtual void send(WireMessage msg) = 0;
        virtual std::optional<WireMessage> poll() = 0;
        virtual bool has_pending() const = 0;
    };

    // Simple in-process transport (single-node). Thread-safe for MPMC use.
    class InProcTransport final : public ITransport
    {
    public:
        void send(WireMessage msg) override
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_queue.push_back(std::move(msg));
        }

        std::optional<WireMessage> poll() override
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_queue.empty())
            {
                return std::nullopt;
            }
            WireMessage out = std::move(m_queue.front());
            m_queue.pop_front();
            return out;
        }

        bool has_pending() const override
        {
            std::lock_guard<std::mutex> lk(m_mu);
            return !m_queue.empty();
        }

    private:
        mutable std::mutex m_mu;
        std::deque<WireMessage> m_queue;
    };
}
