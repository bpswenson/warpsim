# airplanes_missile_aoi_demo

This document explains what is being modeled in the `airplanes_missile_aoi_demo` example and how it maps onto warpsim concepts (LPs, entities, events, and AOI).

## Goal

Model a very small “combat” scenario:

- Airplanes move around inside a region.
- Each region is simulated by a separate Logical Process (LP).
- Airplanes can "see" airplanes in other regions using an Area-Of-Interest (AOI) service.
- When a region detects an airplane owned by another region, it fires a missile at it.
- Missile hits apply damage on the target’s owner LP and generate a committed output record.

This is intentionally toy-model behavior; the focus is showing:

- Cross-LP interaction via events
- Entity state + rollback-safe updates
- AOI as a reusable service LP
- MPI wiring (`lpToRank`, `MpiTransport`) for multi-rank runs

## LPs in this demo

### 1) Region LPs (the “simulation partitions”)

There are `--regions N` Region LPs with ids `1..N`.

Each Region LP owns a set of airplane entities:

- Airplane entity ids are constructed as:

  - high 32 bits: owning Region LP id
  - low 32 bits: local airplane index

So ownership is embedded in the id. The helper `owner_lp(entityId)` simply decodes the owning Region LP id from the high bits.

Responsibilities of a Region LP:

- Move all locally-owned airplanes every tick.
- Publish airplane positions to the AOI service via AOI update events.
- Periodically issue one AOI query (“radar scan”) to find nearby airplanes.
- If the AOI result includes an airplane owned by a *different* Region LP, fire a missile (send a `Missile_Hit` event) to the target’s owner.
- On receiving a `Missile_Hit`, apply damage to the local target airplane.
- Emit committed output for hits so runs can be summarized/hashed.

### 2) AOI LP (the “spatial index service”)

There is a single `AoiLP` instance that:

- Accepts position updates (`Aoi_Update`) for entities.
- Answers queries (`Aoi_Query`) and returns results (`Aoi_Result`).

The AOI LP is a *service* LP: it does not own airplanes; it only maintains a mapping of `EntityId -> Vec3 position`.

The AOI result returns a deterministic, sorted list of entity ids that match the query shape. In this demo, Region LPs use sphere queries.

## Entities in this demo

Airplanes are modeled as entities stored in a `World` attached to each Region LP.

Entity state includes:

- Position and velocity
- Hit points (`hp`) and alive/dead flag
- Counters like shots fired / hits taken / kills (purely for instrumentation)

### Rollback safety

Region LPs update airplane state through `WorldTxn`.

- `WorldTxn::write(entityId, type)` calls `ctx.request_write(entityId)` before mutating the entity.
- That tells the kernel to snapshot the entity so the LP can be rolled back correctly.

Important: if an LP has *any* additional mutable control state outside entities, that must be captured in the LP’s `save_state/load_state`. This demo snapshots its small control variable that gates tick progression.

## Events and message flow

This demo uses these event kinds:

- `Region_Tick`: internal self-event that advances a Region LP by one time step.
- `Aoi_Update`: a Region LP publishes a plane position to AOI.
- `Aoi_Query`: a Region LP asks AOI for nearby entities.
- `Aoi_Result`: AOI replies with the matching entity ids.
- `Missile_Hit`: a Region LP applies damage to a target airplane on the owner LP.
- `Commit_Hit`: committed output record emitted on hit application.

A typical cycle:

1. Region LP processes `Region_Tick` at time `t`.
2. It moves each local airplane and sends `Aoi_Update` events for their positions.
3. Periodically, it sends one `Aoi_Query` event.
4. AOI processes the query and replies with `Aoi_Result`.
5. The Region LP selects one cross-LP target from the result and sends a `Missile_Hit` to the target owner.
6. Target owner processes `Missile_Hit`, applies damage, and emits `Commit_Hit` via `ctx.emit_committed()`.

## Determinism and committed output

The demo emits committed output for each hit. The top-level program hashes these committed records (order-independent combine) and prints a final `hash=` summary.

Committed output is the recommended way to “observe” simulation behavior in optimistic execution: direct printing during event execution is not rollback-safe.

## MPI mapping

When built with MPI enabled and launched with `mpiexec -n K`, the demo uses:

- `MpiTransport` for cross-rank wire messages
- `SimulationConfig::lpToRank` to map each LP id to an owning MPI rank
- MPI reductions for termination / GVT bookkeeping

Region LPs are only instantiated on the rank that owns them.

## How to run

Single-process:

- `./build/examples/warpdes_airplanes_missile_aoi_demo --regions 4 --planes-per-region 8 --end 60 --seed 1`

MPI:

- `mpiexec -n 3 ./build/examples/warpdes_airplanes_missile_aoi_demo --regions 4 --planes-per-region 8 --end 60 --seed 1`

You can also pass `--verify --expected-hash <H>` if you want the program to enforce a specific hash value.
