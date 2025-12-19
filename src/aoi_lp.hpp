#pragma once

#include "lp.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace warpsim
{
    struct Vec3
    {
        double x = 0;
        double y = 0;
        double z = 0;
    };

    inline double dot(Vec3 a, Vec3 b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline Vec3 sub(Vec3 a, Vec3 b) noexcept
    {
        return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
    }

    inline double norm2(Vec3 a) noexcept
    {
        return dot(a, a);
    }

    // Generic area-of-interest LP.
    //
    // Pattern:
    // - Entities publish their position via Update events.
    // - Any LP can ask "who is in range/visible" via a Query event.
    // - AOI replies to ev.src with a deterministic list of EntityId.
    //
    // This is the PDES-friendly replacement for "broadcast": the fanout is produced
    // by a deterministic query against a rollback-safe spatial index.
    class AoiLP final : public ILogicalProcess
    {
    public:
        enum class QueryShape : std::uint32_t
        {
            Sphere = 1,
            Cone = 2,
        };

        struct Config
        {
            LPId id = 0;

            // Used with request_write() to snapshot AOI state before mutation.
            EntityId stateEntityId = 0;

            std::uint32_t updateKind = 0;
            std::uint32_t queryKind = 0;
            std::uint32_t resultKind = 0;

            // If true, excludes the querying entity id from results (when non-zero).
            bool excludeSelf = true;
        };

        struct UpdateArgs
        {
            EntityId entity = 0;
            Vec3 pos{};
        };

        struct QueryArgs
        {
            std::uint64_t requestId = 0;
            EntityId self = 0;

            QueryShape shape = QueryShape::Sphere;

            Vec3 origin{};

            // Sphere radius.
            double radius = 0;

            // Cone query:
            // - direction should be normalized by the model (but we don't require it)
            // - range is max distance
            // - cosHalfAngle is cos(theta) for half-angle theta
            Vec3 direction{};
            double range = 0;
            double cosHalfAngle = 1.0;
        };

        struct ResultHeader
        {
            std::uint64_t requestId = 0;
            std::uint32_t count = 0;
        };

        struct Result
        {
            std::uint64_t requestId = 0;
            std::vector<EntityId> entities;
        };

        static Result decode_result_bytes(std::span<const std::byte> bytes)
        {
            if (bytes.size() < sizeof(ResultHeader))
            {
                throw std::runtime_error("AoiLP: bad result payload");
            }

            ResultHeader h{};
            std::memcpy(&h, bytes.data(), sizeof(h));

            const std::size_t expected = sizeof(h) + static_cast<std::size_t>(h.count) * sizeof(EntityId);
            if (bytes.size() != expected)
            {
                throw std::runtime_error("AoiLP: bad result payload size");
            }

            Result out;
            out.requestId = h.requestId;
            out.entities.resize(h.count);
            if (h.count != 0)
            {
                std::memcpy(out.entities.data(), bytes.data() + sizeof(h), out.entities.size() * sizeof(EntityId));
            }
            return out;
        }

        explicit AoiLP(Config cfg) : m_cfg(std::move(cfg))
        {
            if (m_cfg.id == 0)
            {
                throw std::runtime_error("AoiLP: id must be non-zero");
            }
            if (m_cfg.stateEntityId == 0)
            {
                throw std::runtime_error("AoiLP: stateEntityId must be non-zero");
            }
            if (m_cfg.updateKind == 0 || m_cfg.queryKind == 0 || m_cfg.resultKind == 0)
            {
                throw std::runtime_error("AoiLP: update/query/result kinds must be non-zero");
            }
        }

        LPId id() const noexcept override { return m_cfg.id; }

        void on_event(const Event &ev, IEventContext &ctx) override
        {
            if (ev.payload.kind == m_cfg.updateKind)
            {
                const UpdateArgs a = decode_trivial_<UpdateArgs>(ev);
                ctx.request_write(m_cfg.stateEntityId);
                m_pos[a.entity] = a.pos;
                return;
            }

            if (ev.payload.kind == m_cfg.queryKind)
            {
                const QueryArgs q = decode_trivial_<QueryArgs>(ev);

                std::vector<EntityId> hits;
                hits.reserve(64);

                if (q.shape == QueryShape::Sphere)
                {
                    const double r2 = q.radius * q.radius;
                    for (const auto &[entity, pos] : m_pos)
                    {
                        if (m_cfg.excludeSelf && q.self != 0 && entity == q.self)
                        {
                            continue;
                        }
                        const Vec3 d = sub(pos, q.origin);
                        if (norm2(d) <= r2)
                        {
                            hits.push_back(entity);
                        }
                    }
                }
                else if (q.shape == QueryShape::Cone)
                {
                    const double range2 = q.range * q.range;
                    for (const auto &[entity, pos] : m_pos)
                    {
                        if (m_cfg.excludeSelf && q.self != 0 && entity == q.self)
                        {
                            continue;
                        }
                        const Vec3 d = sub(pos, q.origin);
                        const double d2 = norm2(d);
                        if (d2 > range2)
                        {
                            continue;
                        }
                        // cos(angle) = dot(d,dir) / (|d|*|dir|). We avoid sqrt by comparing
                        // dot(d,dir)^2 >= cos^2 * |d|^2 * |dir|^2.
                        const double dd = dot(d, q.direction);
                        if (dd <= 0)
                        {
                            continue;
                        }
                        const double dir2 = norm2(q.direction);
                        if (dir2 == 0)
                        {
                            continue;
                        }
                        const double lhs = dd * dd;
                        const double rhs = (q.cosHalfAngle * q.cosHalfAngle) * d2 * dir2;
                        if (lhs >= rhs)
                        {
                            hits.push_back(entity);
                        }
                    }
                }
                else
                {
                    throw std::runtime_error("AoiLP: unknown query shape");
                }

                // Deterministic ordering.
                std::sort(hits.begin(), hits.end());

                Event out;
                out.ts = ev.ts;
                out.src = m_cfg.id;
                out.dst = ev.src;
                out.target = 0;
                out.payload.kind = m_cfg.resultKind;
                out.payload.bytes = encode_result_(q.requestId, hits);
                ctx.send(std::move(out));
                return;
            }

            // Ignore unknown kinds to keep AOI reusable.
        }

        ByteBuffer save_state() const override
        {
            // Format: u32 n, then entries {u64 entity, Vec3}.
            ByteBuffer out;
            auto append = [&](const void *p, std::size_t n)
            {
                const auto *b = static_cast<const std::byte *>(p);
                out.insert(out.end(), b, b + n);
            };

            const std::uint32_t n = static_cast<std::uint32_t>(m_pos.size());
            append(&n, sizeof(n));
            for (const auto &[entity, pos] : m_pos)
            {
                append(&entity, sizeof(entity));
                append(&pos, sizeof(pos));
            }
            return out;
        }

        void load_state(std::span<const std::byte> state) override
        {
            auto read = [&](std::size_t &off, void *dst, std::size_t n)
            {
                if (off + n > state.size())
                {
                    throw std::runtime_error("AoiLP: bad state");
                }
                std::memcpy(dst, state.data() + off, n);
                off += n;
            };

            std::size_t off = 0;
            std::uint32_t n = 0;
            read(off, &n, sizeof(n));

            m_pos.clear();
            for (std::uint32_t i = 0; i < n; ++i)
            {
                EntityId e = 0;
                Vec3 p{};
                read(off, &e, sizeof(e));
                read(off, &p, sizeof(p));
                m_pos.emplace(e, p);
            }
        }

    private:
        template <class T>
        static T decode_trivial_(const Event &ev)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (ev.payload.bytes.size() != sizeof(T))
            {
                throw std::runtime_error("AoiLP: bad payload size");
            }
            T out{};
            std::memcpy(&out, ev.payload.bytes.data(), sizeof(T));
            return out;
        }

        static ByteBuffer encode_result_(std::uint64_t requestId, const std::vector<EntityId> &entities)
        {
            ResultHeader h;
            h.requestId = requestId;
            h.count = static_cast<std::uint32_t>(entities.size());

            ByteBuffer out;
            out.resize(sizeof(ResultHeader) + entities.size() * sizeof(EntityId));
            std::memcpy(out.data(), &h, sizeof(ResultHeader));
            if (!entities.empty())
            {
                std::memcpy(out.data() + sizeof(ResultHeader), entities.data(), entities.size() * sizeof(EntityId));
            }
            return out;
        }

        Config m_cfg;
        std::map<EntityId, Vec3> m_pos;
    };
}
