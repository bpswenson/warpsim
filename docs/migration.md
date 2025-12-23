# Migration (directory routing + policy API)

This doc describes WarpSim’s **optimistic entity migration** mechanism and the API surface intended for model-defined migration criteria.

Migration has two separate concerns:

- **Addressing / routing**: How does an event reach the right LP if the entity can move?
- **Criteria / policy**: When should an entity move, and where should it go?

WarpSim supports migration by letting model code address **entities** (not LPs) and optionally routing those targeted events through a **directory LP** that forwards to the correct owner-at-time.

## 1) Addressing: LP vs entity

- **LP-addressed events**: set `Event.dst` and usually keep `Event.target = 0`.
- **Entity-targeted (migration-safe)**: set `Event.target = entityId` and use a routing mechanism so the kernel can resolve the owner at `ev.ts`.

Modeler-facing helpers:

- `modeling::make_targeted_event(...)`
- `modeling::send_targeted(...)`

These intentionally do *not* require the caller to know the destination LP.

## 2) Enabling routing: directory LP

To route targeted events when migration is enabled:

1. Set `SimulationConfig::directoryForEntity` so the kernel knows which directory LP to send targeted events to.
2. Add a `DirectoryLP` instance to the simulation.

Example (host-side setup):

```cpp
warpsim::SimulationConfig cfg;
cfg.rank = rank;

cfg.directoryForEntity = [](warpsim::EntityId) -> warpsim::LPId {
    return /*directory LP id*/ 100;
};

auto dirCfg = warpsim::optimistic_migration::make_default_directory_config(
    /*directory LP id*/ 100,
    /*stateEntityId*/   0xD1EC7001u);

// Optional: default owner before any ownership updates exist.
dirCfg.defaultOwner = [](warpsim::EntityId, warpsim::TimeStamp) -> warpsim::LPId {
    return /*initial owner*/ 1;
};

warpsim::Simulation sim(cfg, transport);
sim.add_lp(std::make_unique<warpsim::DirectoryLP>(dirCfg));
```

How routing works:

- When `SimulationConfig::directoryForEntity` is set and an outgoing event has `target != 0`, the kernel routes it to the directory LP.
- The directory LP forwards it (still as a targeted event) to the correct owner-at-time.

## 2.1) What actually happens to a targeted event (end-to-end)

This section answers a common modeling question:

> When an entity migrates, how do “events tied to the entity” migrate?

In WarpSim, events do **not** “move between per-entity inboxes”. Instead, targeted events are routed by **entity identity + timestamp**.

### A) The event is “tied to an entity” via `Event.target`

An event is considered “entity-targeted” if:

- `Event.target != 0` (the entity id)

Model code typically sends these via:

- `modeling::send_targeted(sink, ts, srcLp, entityId, kind, payload)`

These helpers intentionally use `dst=UnsetDst` internally; the kernel will fill in a real destination.

### B) The kernel routes the event to either a directory or a known owner

In `Simulation::send`:

1. If `SimulationConfig::directoryForEntity` is set, then for any `target != 0` event, the kernel sets `ev.dst = directoryForEntity(ev.target)` (unless the sender is already that directory LP).
2. Otherwise (no directory), if the kernel has a known owner in its owner map, it sets `ev.dst = owner`.
3. If neither exists and `dst` is still unset, the kernel throws (targeted events must be routable).

So a targeted event’s “address” is not `dst=someRegionLp`; its address is `target=entityId`, and the kernel resolves `dst`.

### C) The directory forwards based on owner-at-time

The `DirectoryLP` holds a rollback-safe ownership timeline. Conceptually it answers:

$$\text{owner} = \text{owner\_at}(entityId, eventTimeStamp)$$

When the directory receives a targeted event that is **not** an ownership update:

- It computes `owner_at(ev.target, ev.ts)`
- It forwards the event to that owner by setting `dst = owner`
- It sets a fresh `(uid, sequence)` so that anti-messages and ordering remain correct for the forward

### D) What does *not* happen: there is no global “per-entity event list”

WarpSim does not keep a permanent list of “all future events for entity E”.

- The kernel maintains a normal global pending queue (a timestamp-ordered priority queue).
- The directory maintains ownership history (who owns what at each timestamp), not a queue of future events.

The only time you’ll see something that looks like a per-entity list is the optional **install-before-use buffering** described below, and even then it is temporary and keyed by `(destination LP, entity)`.

## 3) Applying an ownership change (update-owner + install)

Ownership changes are model-driven. The model emits an **update-owner** event to the directory.

The default optimistic-migration helper functions live in [src/optimistic_migration.hpp](../src/optimistic_migration.hpp):

- `send_update_owner(...)`
- `make_update_owner_payload(...)`

The payload contains:

- entity id
- new owner LP id
- arbitrary model-defined state bytes

On update-owner, the directory:

- records `(entity, effectiveTimestamp) -> newOwner` in a rollback-safe timeline
- optionally emits an **install** event to `newOwner` carrying the state bytes

Your owner LP should handle install events (default kind `optimistic_migration::DefaultInstallKind`) and restore the entity state.

### Ownership update and install are just normal events

An ownership change is represented as an event at the **cutover timestamp**:

- A model sends an update-owner event to the directory at `effectiveTs`.
- The directory processes it like any other event at `effectiveTs`.
- If configured to `sendInstall`, the directory sends an install event to `newOwner` at exactly the same `effectiveTs`.

This design matters for Time Warp:

- If the update-owner event rolls back, the ownership timeline rolls back.
- Any forwarded traffic that depended on it will be corrected by rollback + anti-messages.

### Event ordering at the cutover timestamp

If you have a lot of simultaneous “real” events at exactly the same timestamp as the update-owner/install cutover, you may want a deterministic rule like:

> install / update-owner should be processed before “use the entity” events at the same timestamp.

WarpSim supports this as an opt-in kernel feature via `SimulationConfig::isControlEvent` (see section 3.2).

Otherwise, order is still deterministic, but it will follow the kernel’s standard ordering rules.

## 3.1) The tricky case: targeted events arriving before install

In an optimistic migration setup, the directory can forward a targeted event to an LP that does not yet have the entity installed (because the install event is a separate message and the overall run may be speculative).

This can present as a model crash like:

- `World: unknown EntityId=...`

This is not a “migration is broken” signal by itself; it can be a normal optimistic transient that will be corrected by rollback.

You have two options:

### Option A (recommended): enable install-before-use buffering in the kernel

In the host config:

```cpp
cfg.enableInstallBeforeUseBuffering = true;
cfg.installEventKind = warpsim::optimistic_migration::DefaultInstallKind;
```

Semantics:

- If `enableInstallBeforeUseBuffering` is true, and the model throws exactly `World: unknown EntityId=...` while processing a **targeted** event, the kernel defers that event instead of crashing.
- Deferred events are stored temporarily in a vector keyed by `(destination LP, entityId)`.
- When an install event for that `(lp, entityId)` is successfully processed, the kernel re-enqueues the deferred events (sorted in timestamp order).

Important clarifications:

- This is **not** a permanent per-entity inbox; it is only a guardrail for missing-install transients.
- It is also deliberately narrow: it only triggers on the specific `World` “unknown entity” error, to avoid hiding real model bugs.
- If the simulation terminates while deferred events still exist, the kernel throws (indicating an install never arrived / was never successfully processed).

### Option B: handle missing entities in the model

The model can defensively create placeholder entities on first contact.

This is sometimes useful for prototyping, but it tends to leak engine semantics into model code and is easy to get subtly wrong.
If your goal is to make model authors’ lives easier, prefer kernel buffering.

## 3.2) Deterministic control-event priority at the same timestamp

If you want the kernel to enforce a deterministic “control events first” rule at the same `ts.time`, configure:

```cpp
cfg.isControlEvent = [](const warpsim::Event &ev) {
    return ev.payload.kind == warpsim::optimistic_migration::DefaultUpdateOwnerKind ||
                 ev.payload.kind == warpsim::optimistic_migration::DefaultInstallKind ||
                 ev.payload.kind == warpsim::optimistic_migration::DefaultFossilCollectKind;
};
```

When `isControlEvent` is set, the kernel assigns `ts.sequence` so that:

- control events at a given `ts.time` sort before
- normal events at that same `ts.time`

This remains deterministic across MPI rank-count changes, because the sequence assignment is per-LP and tied to LP identity, not MPI rank.

See [examples/entity_migration_demo.cpp](../examples/entity_migration_demo.cpp) for a minimal end-to-end example.

## 4) Extensible migration criteria: policy API

WarpSim intentionally does not pick a single migration heuristic. Different models want different criteria (load, locality, AOI overlap, contention, etc.).

The intended extension point is:

- `optimistic_migration::MigrationInputs`
- `optimistic_migration::DecideNewOwnerFn`
- `optimistic_migration::maybe_migrate(...)`

A policy is simply a callback:

```cpp
using DecideNewOwnerFn = std::function<std::optional<LPId>(const MigrationInputs&)>;
```

`maybe_migrate(...)` calls the policy and, if it chooses a new owner, emits the update-owner control event.

### Minimal policy example

```cpp
const warpsim::optimistic_migration::DecideNewOwnerFn policy =
  [](const warpsim::optimistic_migration::MigrationInputs& in) -> std::optional<warpsim::LPId> {
      // Demo heuristic: after two local “work units”, migrate from LP1 to LP2.
      if (in.currentOwner == 1 && in.localWork == 2) {
          return static_cast<warpsim::LPId>(2);
      }
      return std::nullopt;
  };

// Called by the current owner, because it is the one with the state bytes.
const warpsim::optimistic_migration::MigrationInputs in{
    .entity = entityId,
    .currentOwner = selfLp,
    .self = selfLp,
    .now = ev.ts,
    .localWork = localWorkCounter,
    .remoteWork = 0,
};

const warpsim::TimeStamp effectiveTs{ev.ts.time + 2, 0};
const auto stateBytes = /* serialize entity state */;

(void)warpsim::optimistic_migration::maybe_migrate(ctx,
    effectiveTs,
    /*src=*/selfLp,
    /*directoryLp=*/directoryLpId,
    /*updateOwnerKind=*/warpsim::optimistic_migration::DefaultUpdateOwnerKind,
    stateBytes,
    in,
    policy);
```

Notes:

- Only the entity’s current owner should attempt to migrate, because it owns the authoritative state bytes.
- Use a future `effectiveTs` if you want to avoid “change owner at the exact same timestamp” ambiguity.

## 5) Verifying migrations happened

There are two recommended ways to verify migrations.

### A) Rollback-safe “migration logs” via committed output (recommended)

The default directory config enables a committed-output record whenever an update-owner event is applied:

- kind: `optimistic_migration::DefaultMigrationLogKind`
- bytes: `optimistic_migration::MigrationLogRecord`

To print these, handle them in `SimulationConfig::committedSink`:

```cpp
cfg.committedSink = [](warpsim::RankId rank, warpsim::LPId lp, warpsim::TimeStamp ts, const warpsim::Payload& p) {
    if (p.kind == warpsim::optimistic_migration::DefaultMigrationLogKind) {
        const auto r = warpsim::optimistic_migration::decode_migration_log_payload(p);
        std::cerr << "[rank=" << rank << "][lp=" << lp << "] t=" << ts.time
                  << " MIGRATE entity=" << r.entity
                  << " " << r.oldOwner << "->" << r.newOwner
                  << (r.sentInstall ? " (install)" : "")
                  << "\n";
    }
};
```

This is rollback-safe and deterministic: it prints only once the directory’s update-owner event becomes irrevocable.

### B) Trace-level kernel routing logs

If you want to confirm that targeted events are being routed through the directory, enable trace logs:

```cpp
cfg.logLevel = warpsim::LogLevel::Trace;
```

This prints kernel messages such as "route targeted event via directory".

## 6) Reference implementations

- In-proc migration + policy demo: [examples/entity_migration_demo.cpp](../examples/entity_migration_demo.cpp)
- MPI rank-count invariance test for policy-driven migration: [tests/mpi_entity_migration_policy_test.cpp](../tests/mpi_entity_migration_policy_test.cpp)

### Example: putting it together (directory + buffering + priority)

This mirrors what the regional airport migration demo does:

```cpp
warpsim::SimulationConfig cfg;
cfg.directoryForEntity = [](warpsim::EntityId) { return kDirectoryLp; };

// Install-before-use semantics: avoid model-side placeholder hacks.
cfg.enableInstallBeforeUseBuffering = true;
cfg.installEventKind = warpsim::optimistic_migration::DefaultInstallKind;

// Ensure update-owner/install are ordered before “use entity” events at the same timestamp.
cfg.isControlEvent = [](const warpsim::Event &ev) {
    return ev.payload.kind == warpsim::optimistic_migration::DefaultUpdateOwnerKind ||
                 ev.payload.kind == warpsim::optimistic_migration::DefaultInstallKind ||
                 ev.payload.kind == warpsim::optimistic_migration::DefaultFossilCollectKind;
};
```
