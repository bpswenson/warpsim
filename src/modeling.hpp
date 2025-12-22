#pragma once

#include "common.hpp"
#include "event.hpp"
#include "lp.hpp"
#include "random.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace warpsim
{
    namespace modeling
    {
        inline std::string event_identity_string(const Event &ev)
        {
            // Keep this stable and human-readable (used only in exception messages).
            return "ts=(" + std::to_string(ev.ts.time) + "," + std::to_string(ev.ts.sequence) + ")" +
                   " src=" + std::to_string(ev.src) +
                   " uid=" + std::to_string(static_cast<std::uint64_t>(ev.uid)) +
                   " kind=" + std::to_string(ev.payload.kind) +
                   (ev.isAnti ? " anti=1" : " anti=0");
        }

        template <class T>
        inline Payload pack(std::uint32_t kind, const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "modeling::pack requires trivially copyable T");
            Payload p;
            p.kind = kind;
            p.bytes = bytes_from_trivially_copyable(value);
            return p;
        }

        template <class T>
        inline T unpack(const Payload &p, std::uint32_t expectedKind, const char *what = "payload")
        {
            static_assert(std::is_trivially_copyable_v<T>, "modeling::unpack requires trivially copyable T");
            if (p.kind != expectedKind)
            {
                throw std::runtime_error(std::string("modeling::unpack: wrong kind for ") + what +
                                         " (expected=" + std::to_string(expectedKind) +
                                         " got=" + std::to_string(p.kind) + ")");
            }
            if (p.bytes.size() != sizeof(T))
            {
                throw std::runtime_error(std::string("modeling::unpack: wrong size for ") + what +
                                         " (expected=" + std::to_string(sizeof(T)) +
                                         " got=" + std::to_string(p.bytes.size()) + ")");
            }
            T out{};
            std::memcpy(&out, p.bytes.data(), sizeof(T));
            return out;
        }

        template <class T>
        inline T unpack(const Event &ev, std::uint32_t expectedKind, const char *what = "event payload")
        {
            try
            {
                return unpack<T>(ev.payload, expectedKind, what);
            }
            catch (const std::exception &e)
            {
                throw std::runtime_error(std::string(e.what()) + " @ " + event_identity_string(ev));
            }
        }

        // RAII helper to make rollback-safe mutation harder to forget.
        // Usage: `auto _ = modeling::write(ctx, entityId);` then mutate the entity state.
        class WriteGuard final
        {
        public:
            WriteGuard(IEventContext &ctx, EntityId entity) : m_ctx(&ctx), m_entity(entity)
            {
                m_ctx->request_write(m_entity);
            }

        private:
            IEventContext *m_ctx = nullptr;
            EntityId m_entity = 0;
        };

        inline WriteGuard write(IEventContext &ctx, EntityId entity) { return WriteGuard(ctx, entity); }

        // --- Typed event helpers -------------------------------------------------
        // These helpers are meant to reduce boilerplate and make common mistakes loud:
        // - wrong kind
        // - wrong payload size
        // - ad-hoc memcpy and "magic numbers" scattered around models

        template <class T>
        inline Event make_event(TimeStamp ts,
                                LPId src,
                                LPId dst,
                                std::uint32_t kind,
                                const T &args,
                                EntityId target = 0,
                                EventUid uid = 0)
        {
            Event ev;
            ev.ts = ts;
            ev.src = src;
            ev.dst = dst;
            ev.target = target;
            ev.uid = uid; // usually leave 0 and let the kernel assign
            ev.payload = pack(kind, args);
            return ev;
        }

        inline Event make_event(TimeStamp ts,
                                LPId src,
                                LPId dst,
                                Payload payload,
                                EntityId target = 0,
                                EventUid uid = 0)
        {
            Event ev;
            ev.ts = ts;
            ev.src = src;
            ev.dst = dst;
            ev.target = target;
            ev.uid = uid;
            ev.payload = std::move(payload);
            return ev;
        }

        template <class T>
        inline void send(IEventSink &sink,
                         TimeStamp ts,
                         LPId src,
                         LPId dst,
                         std::uint32_t kind,
                         const T &args,
                         EntityId target = 0,
                         EventUid uid = 0)
        {
            sink.send(make_event(ts, src, dst, kind, args, target, uid));
        }

        inline void send(IEventSink &sink,
                         TimeStamp ts,
                         LPId src,
                         LPId dst,
                         Payload payload,
                         EntityId target = 0,
                         EventUid uid = 0)
        {
            sink.send(make_event(ts, src, dst, std::move(payload), target, uid));
        }

        // Targeted (entity-addressed) events.
        //
        // In an optimistic migration setup, the destination LP can change over time.
        // The kernel can route based on `ev.target` (via directory indirection and/or
        // an established owner map), so models should generally address *entities*, not LPs.
        //
        // These helpers intentionally do NOT require a dst LP. They set dst=0 as a sentinel;
        // Simulation::send is expected to fill it in when `target != 0`.
        inline constexpr LPId UnsetDst = std::numeric_limits<LPId>::max();

        template <class T>
        inline Event make_targeted_event(TimeStamp ts,
                                         LPId src,
                                         EntityId target,
                                         std::uint32_t kind,
                                         const T &args,
                                         EventUid uid = 0)
        {
            return make_event(ts, src, /*dst=*/UnsetDst, kind, args, target, uid);
        }

        inline Event make_targeted_event(TimeStamp ts,
                                         LPId src,
                                         EntityId target,
                                         Payload payload,
                                         EventUid uid = 0)
        {
            return make_event(ts, src, /*dst=*/UnsetDst, std::move(payload), target, uid);
        }

        template <class T>
        inline void send_targeted(IEventSink &sink,
                                  TimeStamp ts,
                                  LPId src,
                                  EntityId target,
                                  std::uint32_t kind,
                                  const T &args,
                                  EventUid uid = 0)
        {
            sink.send(make_targeted_event(ts, src, target, kind, args, uid));
        }

        inline void send_targeted(IEventSink &sink,
                                  TimeStamp ts,
                                  LPId src,
                                  EntityId target,
                                  Payload payload,
                                  EventUid uid = 0)
        {
            sink.send(make_targeted_event(ts, src, target, std::move(payload), uid));
        }

        template <class T>
        inline T require(const Event &ev, std::uint32_t expectedKind, const char *what = "event")
        {
            // Equivalent to unpack<T>(ev, expectedKind), but reads nicer in model code.
            return unpack<T>(ev, expectedKind, what);
        }

        template <class T>
        inline void emit_committed(IEventContext &ctx, TimeStamp ts, std::uint32_t kind, const T &value)
        {
            ctx.emit_committed(ts, pack(kind, value));
        }

        // Deterministic model RNG keyed by (seed, lpId, stream, event src, event uid, draw).
        inline std::uint64_t rng_u64(std::uint64_t seed, LPId lp, const Event &ev, std::uint32_t stream, std::uint32_t draw = 0) noexcept
        {
            return warpsim::rng_u64(seed, lp, stream, ev.src, ev.uid, draw);
        }

        inline double rng_unit_double(std::uint64_t seed, LPId lp, const Event &ev, std::uint32_t stream, std::uint32_t draw = 0) noexcept
        {
            return warpsim::rng_unit_double(seed, lp, stream, ev.src, ev.uid, draw);
        }

        inline std::uint64_t rng_u64_range(std::uint64_t seed,
                                           LPId lp,
                                           const Event &ev,
                                           std::uint32_t stream,
                                           std::uint64_t lo,
                                           std::uint64_t hi,
                                           std::uint32_t draw = 0) noexcept
        {
            return warpsim::rng_u64_range(seed, lp, stream, ev.src, ev.uid, lo, hi, draw);
        }
    }
}
