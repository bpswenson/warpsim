#pragma once

#include "lp.hpp"

#include <cstring>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace warpsim
{
    template <class ExtraContext>
    class EventDispatcher
    {
    public:
        using Handler = std::function<void(const Event &, IEventContext &, ExtraContext &)>;

        void register_handler(std::uint32_t eventTypeId, Handler handler)
        {
            if (!handler)
            {
                throw std::runtime_error("register_handler: null handler");
            }
            auto [it, inserted] = m_handlers.emplace(eventTypeId, std::move(handler));
            if (!inserted)
            {
                throw std::runtime_error("register_handler: duplicate eventTypeId");
            }
        }

        void dispatch(const Event &ev, IEventContext &ctx, ExtraContext &extra) const
        {
            auto it = m_handlers.find(ev.payload.kind);
            if (it == m_handlers.end())
            {
                throw std::runtime_error("dispatch: unknown eventTypeId");
            }
            it->second(ev, ctx, extra);
        }

        template <class Args>
        void register_trivial(std::uint32_t eventTypeId, std::function<void(EntityId target, const Args &args, const Event &ev, IEventContext &ctx, ExtraContext &extra)> fn)
        {
            static_assert(std::is_trivially_copyable_v<Args>, "Args must be trivially copyable for register_trivial");
            register_handler(eventTypeId, [fn = std::move(fn)](const Event &ev, IEventContext &ctx, ExtraContext &extra)
                             {
                if (ev.payload.bytes.size() != sizeof(Args))
                {
                    throw std::runtime_error("dispatch: bad args size");
                }
                Args args{};
                std::memcpy(&args, ev.payload.bytes.data(), sizeof(Args));
                fn(ev.target, args, ev, ctx, extra); });
        }

        template <class Args>
        static Payload make_trivial_payload(std::uint32_t eventTypeId, const Args &args)
        {
            static_assert(std::is_trivially_copyable_v<Args>, "Args must be trivially copyable for make_trivial_payload");
            Payload p;
            p.kind = eventTypeId;
            p.bytes = bytes_from_trivially_copyable(args);
            return p;
        }

    private:
        std::unordered_map<std::uint32_t, Handler> m_handlers;
    };
}
