# AOI (Area-of-Interest) LP: a PDES-friendly “broadcast”

The key idea is: **don’t broadcast to everyone**.
Instead, keep a rollback-safe spatial index in a dedicated Logical Process (LP), and ask it “who should receive this?” deterministically.

This pattern is generic for things like radar, explosions, visibility, proximity chat, and any one-to-many mechanic.

## Protocol

The AOI LP supports a simple 3-message protocol:

1. **Update**: entities publish their position to the AOI LP.
2. **Query**: any LP asks “which entities satisfy this shape?” at a specific simulation timestamp.
3. **Result**: AOI replies to the query sender (`ev.src`) with a deterministic list of `EntityId`.

The important PDES property is: the AOI LP is just another LP, so:
- If an update arrives late (straggler), the kernel rolls the AOI LP back.
- If a query is re-executed after rollback, the AOI result list changes deterministically.
- The model then sends/undos downstream fanout via normal Time Warp anti-messages.

## Shapes

`AoiLP` currently supports:

- **Sphere**: origin + radius
  - Explosions, AoE healing, proximity triggers, local chat.
- **Cone**: origin + direction + range + `cosHalfAngle`
  - Radar cones, vision cones, weapon arcs.

## Determinism rules

To keep results stable across rollbacks/ranks:

- The AOI LP stores positions in an ordered container (`std::map`) and sorts final hit lists.
- A query at the same timestamp over the same committed AOI state returns the same list.

## How modelers use it (recommended pattern)

### 1) Create one AOI LP per region (or per sim)

- Add `AoiLP` to the simulation.
- Assign it dedicated event kinds: `updateKind`, `queryKind`, `resultKind`.

### 2) Publish positions

Whenever an entity moves, send an `UpdateArgs{entity, pos}` to the AOI LP at the movement event timestamp.

### 3) Query when you need fanout

For example:

- Radar ping: send a **Cone** query.
- Explosion: send a **Sphere** query.

The AOI result list gives you “who to notify”.

### 4) Fanout by sending *normal* events

Your model LP (often the initiator) sends one event per recipient LP/entity.
This keeps the core kernel simple and makes rollback correct “for free” because the kernel will anti-cancel those fanout events if the query result changes after rollback.

## Common scenarios

- **Explosions**: Query Sphere, then apply damage events to returned entities.
- **Radar / visibility**: Query Cone, then send “spotted” or “track” events.
- **Proximity chat**: Query Sphere around speaker, then send chat events.
- **Collisions**: Query small Sphere around each moving object, then do narrow-phase checks in the mover LP.

## Practical guidance (what makes it easy)

- Keep the AOI LP’s payloads trivially-copyable (they are), and keep all fanout logic in one place (a single “driver/weapon” LP).
- Use deterministic `EntityId` assignment (like `(ownerLPId<<32)|localId`) so hit lists are stable.
- If you want late-message/rollback stress tests, use a throttled transport in the example, but for “smoke tests” prefer FIFO delivery.
