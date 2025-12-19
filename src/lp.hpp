#pragma once

#include "event.hpp"

namespace warpsim
{
    class IEventSink
    {
    public:
        virtual ~IEventSink() = default;
        virtual void send(Event ev) = 0;
    };

    class IEventContext : public IEventSink
    {
    public:
        virtual ~IEventContext() = default;

        // Request write access to an entity by id. The kernel may snapshot the entity
        // the first time this is called per (event, entity).
        virtual void request_write(EntityId id) = 0;
    };

    class ILogicalProcess
    {
    public:
        virtual ~ILogicalProcess() = default;

        virtual LPId id() const noexcept = 0;

        // Called when the LP is about to start processing (optional hook).
        virtual void on_start(IEventSink &) {}

        // Process one event at its timestamp. Must be deterministic.
        virtual void on_event(const Event &ev, IEventContext &ctx) = 0;

        // Optimistic execution requires byte-serializable state for snapshots/rollback.
        virtual ByteBuffer save_state() const = 0;
        virtual void load_state(std::span<const std::byte> state) = 0;

        // Optional per-entity snapshot hooks. If not overridden, the kernel may fall
        // back to LP-wide snapshots.
        virtual bool supports_entity_snapshots() const { return false; }
        virtual ByteBuffer save_entity(EntityId) const { return {}; }
        virtual void load_entity(EntityId, std::span<const std::byte>) {}

        // Optional: invoked when a migration message is applied.
        virtual void on_migration(EntityId, std::span<const std::byte>) {}
    };
}
