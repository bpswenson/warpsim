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
