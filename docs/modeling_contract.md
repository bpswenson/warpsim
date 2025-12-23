# Modeling contract (rules for correct Time Warp models)

This simulator runs a **Time Warp** (optimistic) PDES kernel. Correctness depends on model code following a few strict rules.

If you follow these, your model should produce the **same results** across:

- single-rank runs
- MPI runs with different numbers of ranks
- executions that experience different rollback patterns

## 0) LP vs entity (what this contract assumes)

This contract talks about LPs and entities because they matter for *correctness*:

- **LP (Logical Process)**: the execution container that runs `on_event()`. LPs are the unit the kernel rolls back.
- **Entity**: model state addressed by `EntityId`. If you enable migration/directory routing, entity ownership can move between LPs over time.

Practical implications:

- If you mutate entity state, call `ctx.request_write(entityId)` first.
- If your “destination” can migrate, address the entity (`ev.target`) and let the kernel route; don’t guess an LP.

## 1) Determinism (no hidden nondeterminism)

Inside `ILogicalProcess::on_event()`:

- Do not read wall-clock time.
- Do not use OS randomness (`std::random_device`, `rand()`, etc.).
- Do not depend on thread scheduling (assume single-threaded per rank, but still avoid data races).
- Do not iterate unordered containers in a way that affects behavior (`unordered_map` iteration order is not stable).

If you need randomness, use the patterns in [random.md](random.md).

## 2) Rollback discipline: snapshot before mutation

Optimistic execution means an event may be undone. The kernel can only restore state you asked it to snapshot.

Rule:

- Before you mutate rollback-relevant state, call `ctx.request_write(entityId)`.

Notes:

- If your LP overrides `supports_entity_snapshots()` and implements `save_entity/load_entity`, the kernel can snapshot only the entities you touch.
- Otherwise it falls back to an LP-wide snapshot via `save_state/load_state`.

More detail: [state_store.md](state_store.md).

Tip:

- Use the helper `modeling::write(ctx, entityId)` from [src/modeling.hpp](../src/modeling.hpp) to make this hard to forget.

Notes:

- If your LP supports entity snapshots, request writes for the specific entity IDs you will mutate.
- Otherwise the kernel falls back to an LP-wide snapshot.

If you mutate state without requesting a snapshot first, rollbacks may silently corrupt state.

## 3) Side effects: use committed output

Do **not** perform irreversible side effects inside `on_event()` (printing, file I/O, metrics emission, network calls).
A rollback can cause duplicates.

Instead:

- Record side effects with `ctx.emit_committed(ev.ts, payload)`.
- Provide a `SimulationConfig::committedSink` to consume them.

Tip:

- `modeling::emit_committed(ctx, ev.ts, kind, value)` helps avoid manual packing.

See [committed_output.md](committed_output.md).

## 4) Random numbers: use *logical* RNG keys

A key point in Time Warp:

- The kernel’s `Event.uid` is a **transport identity** used for anti-message matching.
- If a sender rolls back, it may cancel an already-sent event and later re-send a logically equivalent event with a **different** `uid`.

Therefore:

- It’s fine to use the incoming event’s `uid` for *local* per-execution bookkeeping.
- For model randomness that must remain stable across causal chains, prefer a **model-level logical id / RNG key** carried in the event payload (or derived deterministically from payload + stable state).

See [random.md](random.md) and the airport example for a practical pattern.

Tip:

- If you do choose to key randomness off kernel identity, prefer the pair `(ev.src, ev.uid)`.
	The helper `modeling::rng_*` in [src/modeling.hpp](../src/modeling.hpp) does this salting for you.

## 5) Event identity: don’t invent kernel UIDs

- Let the kernel assign `Event.uid` unless you have a very specific reason.
- `Event.uid` is guaranteed unique per sender LP (`Event.src`) and deterministic w.r.t LP identity (the kernel enforces this).
- Anti-message matching uses the pair `(Event.src, Event.uid)`.

For stable model-level identities, put your own ids in the payload.

## 6) Event ordering at the same timestamp

- The kernel orders events by `(TimeStamp, Event.src, Event.uid)`.
- If your model has simultaneous events that must be processed in a specific order, encode that order explicitly (e.g., via payload priorities or additional timestamps), not by relying on container iteration.

## 7) MPI portability

To remain rank-count independent:

- Never use `rank` to make model decisions.
- Use stable identifiers like `LPId`, `EntityId`, and model-level ids.
- Avoid assumptions about message latency or arrival order.

## 7.1) Migration: don’t target a moving LP

If your simulation enables optimistic migration (directory routing / dynamic ownership), the *destination LP* for an entity is not stable.

Guideline:

- Address entities, not LPs: set `ev.target = entityId` and let the kernel (and optional directory LP) route to the correct owner at `ev.ts`.

Tip:

- Use `modeling::send_targeted(...)` / `modeling::make_targeted_event(...)` from [src/modeling.hpp](../src/modeling.hpp) so model code does not need to guess the current owner LP.

Behavioral details:

- Targeted events are routed either via the directory LP (recommended for migration) or via a known owner map.
- In optimistic runs, targeted events may be forwarded speculatively to an LP before the entity install arrives.
- If enabled in the host config, the kernel can buffer targeted events that would otherwise fail with `World: unknown EntityId=...` until the install event is processed.
- If enabled in the host config, the kernel can enforce deterministic “control event first” ordering (e.g., update-owner/install before normal events at the same timestamp).

See [migration.md](migration.md) for the full end-to-end routing flow and configuration examples.

Important:

- Only use targeted-event routing if your run config enables it (e.g., `SimulationConfig::directoryForEntity` is set, or entity ownership is otherwise tracked). Without routing, sending with an unknown destination cannot be made correct.

Criteria:

- How/when to migrate is model-defined. Different models want different heuristics (load, locality, AOI overlap, etc.).
- A practical pattern is: your model computes a decision, captures the entity’s state bytes, then emits a directory “update owner” event.
	The helpers in [src/optimistic_migration.hpp](../src/optimistic_migration.hpp) provide `DecideNewOwnerFn` + `maybe_migrate(...)` to make this pattern easy to extend.

Verification:

- See [migration.md](migration.md) for how to verify migrations via rollback-safe committed output and trace-level routing logs.

## 8) Serialization

Your LP must implement:

- `save_state()` and `load_state()` (or entity snapshot hooks)

These must fully capture rollback-relevant state.

## 9) Recommended patterns

- Broadcast-like behavior: [aoi.md](aoi.md)
- Side effects: [committed_output.md](committed_output.md)
- Rollback-safe randomness: [random.md](random.md)

## 10) Payload hygiene (strongly recommended)

Avoid ad-hoc `memcpy` and unchecked payload sizes in models.

- Use `modeling::pack(kind, args)` to build payloads.
- Use `modeling::require<Args>(ev, expectedKind)` / `modeling::unpack<Args>(...)` to validate kind and size.

These helpers throw exceptions with useful context (timestamp, src, uid) when something is wrong.
