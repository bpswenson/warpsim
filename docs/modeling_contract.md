# Modeling contract (rules for correct Time Warp models)

This simulator runs a **Time Warp** (optimistic) PDES kernel. Correctness depends on model code following a few strict rules.

If you follow these, your model should produce the **same results** across:

- single-rank runs
- MPI runs with different numbers of ranks
- executions that experience different rollback patterns

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

- If your LP supports entity snapshots, request writes for the specific entity IDs you will mutate.
- Otherwise the kernel falls back to an LP-wide snapshot.

If you mutate state without requesting a snapshot first, rollbacks may silently corrupt state.

## 3) Side effects: use committed output

Do **not** perform irreversible side effects inside `on_event()` (printing, file I/O, metrics emission, network calls).
A rollback can cause duplicates.

Instead:

- Record side effects with `ctx.emit_committed(ev.ts, payload)`.
- Provide a `SimulationConfig::committedSink` to consume them.

See [committed_output.md](committed_output.md).

## 4) Random numbers: use *logical* RNG keys

A key point in Time Warp:

- The kernel’s `Event.uid` is a **transport identity** used for anti-message matching.
- If a sender rolls back, it may cancel an already-sent event and later re-send a logically equivalent event with a **different** `uid`.

Therefore:

- It’s fine to use the incoming event’s `uid` for *local* per-execution bookkeeping.
- For model randomness that must remain stable across causal chains, prefer a **model-level logical id / RNG key** carried in the event payload (or derived deterministically from payload + stable state).

See [random.md](random.md) and the airport example for a practical pattern.

## 5) Event identity: don’t invent kernel UIDs

- Let the kernel assign `Event.uid` unless you have a very specific reason.
- UIDs must be globally unique and deterministic w.r.t LP identity (the kernel enforces this).

For stable model-level identities, put your own ids in the payload.

## 6) Event ordering at the same timestamp

- The kernel orders events by `(TimeStamp, Event.uid)`.
- If your model has simultaneous events that must be processed in a specific order, encode that order explicitly (e.g., via payload priorities or additional timestamps), not by relying on container iteration.

## 7) MPI portability

To remain rank-count independent:

- Never use `rank` to make model decisions.
- Use stable identifiers like `LPId`, `EntityId`, and model-level ids.
- Avoid assumptions about message latency or arrival order.

## 8) Serialization

Your LP must implement:

- `save_state()` and `load_state()` (or entity snapshot hooks)

These must fully capture rollback-relevant state.

## 9) Recommended patterns

- Broadcast-like behavior: [aoi.md](aoi.md)
- Side effects: [committed_output.md](committed_output.md)
- Rollback-safe randomness: [random.md](random.md)
