# State store, snapshots, and entities

This repo contains two related but distinct concepts:

1. **Kernel rollback snapshots**: how WarpSim snapshots *LP state* (and optionally *entity state*) so it can roll back.
2. **`StateHistory`** (in [src/state_store.hpp](../src/state_store.hpp)): a small utility for keeping a time-indexed history of byte snapshots.

Most modelers primarily interact with (1). `StateHistory` is a building block you can use inside an LP when you need your own rollback-safe timeline.

## Entities vs storage

An **entity** is identified by `EntityId`. That does *not* imply there is a global “entity store” inside the kernel.

Instead:
- Your model decides where entity state lives (often inside an `ILogicalProcess` as a map keyed by `EntityId`, or via [src/world.hpp](../src/world.hpp)).
- The kernel uses `EntityId` as a *snapshot key* when you call `ctx.request_write(entityId)`.

## How rollback snapshots work

Inside `ILogicalProcess::on_event(ev, ctx)`:

- Call `ctx.request_write(entityId)` **before** mutating rollback-relevant state.
- If your LP implements per-entity snapshots (`supports_entity_snapshots() == true`), then the kernel captures the pre-state via `save_entity(entityId)` the first time you request that entity during the event.
- Otherwise, the kernel falls back to an LP-wide snapshot by calling `save_state()` once per event (on the first write request).

On rollback, the kernel restores:
- LP-wide state via `load_state(...)` (if it used LP snapshots), and/or
- Specific entities via `load_entity(entityId, ...)` (if it used per-entity snapshots).

Practical implication:
- If you have many entities but each event touches only a small subset, **per-entity snapshots can drastically reduce snapshot size and improve performance**.

See the modeling rules in [docs/modeling_contract.md](modeling_contract.md).

## What `StateHistory` is for

`StateHistory` is a simple container that stores:

- a set of `(TimeStamp -> ByteBuffer)` entries
- with helpers to retrieve the latest snapshot at/before a timestamp and to drop history after a rollback point

This is useful when your LP needs its own timeline, e.g.:
- ownership timelines (directory routing)
- per-entity “configuration over time” tables
- caches that must be consistent under rollback

### Rollback safety

If you mutate a `StateHistory` inside an LP, it is just normal LP state.

That means you must still follow the normal snapshot rule:
- call `ctx.request_write(...)` (LP-wide or entity-specific) before mutating the timeline

A common pattern is to dedicate a single `stateEntityId` for an LP’s internal timeline state (as done by `DirectoryLP`).

## `TimeStamp` and ordering

`TimeStamp` is defined in [src/state_store.hpp](../src/state_store.hpp) and used throughout the engine.

- `time` is the primary simulation time.
- `sequence` is a deterministic tie-breaker for events at the same simulation time.

The kernel orders events by `(TimeStamp, Event.src, Event.uid)`.

