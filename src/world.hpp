#pragma once

#include "common.hpp"

#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace warpsim
{
    using EntityTypeId = std::uint32_t;

    struct EntityTypeOps
    {
        EntityTypeId type = 0;
        std::function<void *()> create;
        std::function<void(void *)> destroy;
        std::function<ByteBuffer(const void *)> serialize;
        std::function<void(void *, std::span<const std::byte>)> deserialize;
    };

    class EntityTypeRegistry
    {
    public:
        void register_type(EntityTypeOps ops)
        {
            if (ops.type == 0 || !ops.create || !ops.destroy || !ops.serialize || !ops.deserialize)
            {
                throw std::runtime_error("register_type: incomplete ops");
            }
            auto [it, inserted] = m_ops.emplace(ops.type, std::move(ops));
            if (!inserted)
            {
                throw std::runtime_error("register_type: duplicate EntityTypeId");
            }
        }

        template <class T>
        void register_trivial(EntityTypeId type)
        {
            static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for register_trivial");
            EntityTypeOps ops;
            ops.type = type;
            ops.create = []() -> void *
            { return new T{}; };
            ops.destroy = [](void *p)
            { delete static_cast<T *>(p); };
            ops.serialize = [](const void *p) -> ByteBuffer
            {
                return bytes_from_trivially_copyable(*static_cast<const T *>(p));
            };
            ops.deserialize = [](void *p, std::span<const std::byte> bytes)
            {
                T tmp{};
                if (bytes.size() == sizeof(T))
                {
                    std::memcpy(&tmp, bytes.data(), sizeof(T));
                }
                *static_cast<T *>(p) = tmp;
            };
            register_type(std::move(ops));
        }

        const EntityTypeOps &get(EntityTypeId type) const
        {
            auto it = m_ops.find(type);
            if (it == m_ops.end())
            {
                throw std::runtime_error("Unknown EntityTypeId");
            }
            return it->second;
        }

    private:
        std::unordered_map<EntityTypeId, EntityTypeOps> m_ops;
    };

    class World
    {
    public:
        explicit World(const EntityTypeRegistry &registry) : m_registry(registry) {}

        template <class T>
        void emplace(EntityId id, EntityTypeId type, T value = {})
        {
            const auto &ops = m_registry.get(type);
            std::unique_ptr<void, std::function<void(void *)>> ptr(ops.create(), ops.destroy);
            *static_cast<T *>(ptr.get()) = value;

            EntityRecord rec;
            rec.type = type;
            rec.ptr = std::move(ptr);

            auto [it, inserted] = m_entities.emplace(id, std::move(rec));
            if (!inserted)
            {
                throw std::runtime_error("World::emplace: duplicate EntityId");
            }
        }

        EntityTypeId type_of(EntityId id) const
        {
            return record_(id).type;
        }

        template <class T>
        const T &read(EntityId id, EntityTypeId type) const
        {
            const auto &rec = record_(id);
            if (rec.type != type)
            {
                throw std::runtime_error("World::read: type mismatch");
            }
            return *static_cast<const T *>(rec.ptr.get());
        }

        template <class T>
        T &write(EntityId id, EntityTypeId type)
        {
            auto &rec = record_(id);
            if (rec.type != type)
            {
                throw std::runtime_error("World::write: type mismatch");
            }
            return *static_cast<T *>(rec.ptr.get());
        }

        ByteBuffer serialize_entity(EntityId id) const
        {
            const auto &rec = record_(id);
            const auto &ops = m_registry.get(rec.type);
            return ops.serialize(rec.ptr.get());
        }

        void deserialize_entity(EntityId id, std::span<const std::byte> bytes)
        {
            auto &rec = record_(id);
            const auto &ops = m_registry.get(rec.type);
            ops.deserialize(rec.ptr.get(), bytes);
        }

    private:
        struct EntityRecord
        {
            EntityTypeId type = 0;
            std::unique_ptr<void, std::function<void(void *)>> ptr{nullptr, [](void *) {}};
        };

        const EntityRecord &record_(EntityId id) const
        {
            auto it = m_entities.find(id);
            if (it == m_entities.end())
            {
                throw std::runtime_error("World: unknown EntityId=" + std::to_string(static_cast<std::uint64_t>(id)));
            }
            return it->second;
        }

        EntityRecord &record_(EntityId id)
        {
            auto it = m_entities.find(id);
            if (it == m_entities.end())
            {
                throw std::runtime_error("World: unknown EntityId=" + std::to_string(static_cast<std::uint64_t>(id)));
            }
            return it->second;
        }

        const EntityTypeRegistry &m_registry;
        std::unordered_map<EntityId, EntityRecord> m_entities;
    };

    class IEventContext;

    class WorldTxn
    {
    public:
        WorldTxn(World &world, IEventContext &ctx) : m_world(world), m_ctx(ctx) {}

        template <class T>
        const T &read(EntityId id, EntityTypeId type) const
        {
            return m_world.read<T>(id, type);
        }

        template <class T>
        T &write(EntityId id, EntityTypeId type);

    private:
        World &m_world;
        IEventContext &m_ctx;
    };

    template <class T>
    inline T &WorldTxn::write(EntityId id, EntityTypeId type)
    {
        m_ctx.request_write(id);
        return m_world.write<T>(id, type);
    }
}
