# Randomness in an optimistic Time Warp simulation

In an optimistic (rollback-capable) simulator, random numbers must be:

- **Deterministic** (same inputs produce the same outputs)
- **Rollback-safe** (re-executing an event after rollback produces the same “random” draws)
- **Rank-independent** under MPI (changing rank mapping should not change results)

This repo’s recommended default is a **stateless, counter-based RNG** implemented in [src/random.hpp](../src/random.hpp).

## Recommended pattern: stateless RNG keyed off stable identifiers

Instead of storing a mutable PRNG in your LP, derive random values from:

- `SimulationConfig::seed` (global run seed)
- `LPId` (stable simulation identity; do **not** use MPI rank)
- a **model-level logical RNG key** of the *event you’re currently processing*
- a `stream` id (separate subsystems)
- a `draw` index (0,1,2,...) for multiple draws per event

In many models, the logical RNG key is carried in the event payload (or derived deterministically from payload + stable state).

Example:

```cpp
// Recommended: use the modeler-facing helpers.
#include "modeling.hpp"

// inside on_event(const Event& ev, IEventContext& ctx)
// Prefer a model-level logical key carried in the payload (recommended).
const warpsim::EventUid logicalKey = /* ... */;

// Derive randomness from the logical key (rollback-safe and rank-independent).
const double u = warpsim::rng_unit_double(cfg.seed, /*lp=*/id(), /*stream=*/7, logicalKey, /*draw=*/0);
const std::uint64_t k = warpsim::rng_u64_range(cfg.seed, id(), /*stream=*/7, logicalKey, 10, 20, /*draw=*/1);

// If you do need to key off kernel identity, use (ev.src, ev.uid):
const std::uint64_t r = warpsim::modeling::rng_u64(cfg.seed, id(), ev, /*stream=*/7, /*draw=*/0);
```

### Why you usually should not use `Event.uid` as a logical RNG key

`Event.uid` is a kernel/transport identity used for anti-message matching. The kernel’s anti-match identity is the pair `(ev.src, ev.uid)`.

If a sender rolls back, it may cancel an already-sent event and later re-send a logically equivalent event with a **different** `uid`.
That means a receiver that keys its randomness off `ev.uid` can get different random draws depending on rollback history.

So:

- Use `ev.uid` only for per-execution bookkeeping that doesn’t affect model semantics.
- For model semantics, use a logical RNG key carried in the payload (or derived deterministically).

### Important: don’t key randomness off newly-created outgoing event IDs

Outgoing events created during a handler may get different `uid` values across re-executions (because the kernel must never reuse UIDs).
So, for model randomness, prefer a **payload-carried logical RNG key**.

## Pairing with `Simulation::run_one()` for rollback demos

For demonstrations and tests, it’s often useful to step the kernel one event at a time.
`Simulation::run_one()` lets you do that, and the stateless RNG pattern still works because it keys randomness off the *incoming* event’s identity (typically a model-level logical key from the payload; if you must use kernel identity, use `(ev.src, ev.uid)`).

Typical pattern:

1. Step to process a “later” event.
2. Inject a straggler (earlier timestamp) to force rollback.
3. Keep stepping until quiescence.

See the unit test [tests/run_one_rollback_test.cpp](../tests/run_one_rollback_test.cpp) for a minimal example.

## Alternative (advanced): stateful PRNG in LP state

You can store a PRNG state in your LP, but then it must be part of rollback-able state.
That means:

- If you use entity snapshots, `request_write(entityId)` must happen before any draw that mutates that entity’s RNG state.
- If you use LP-wide snapshots, ensure your LP state serialization includes the RNG state.

This approach is more error-prone, which is why the stateless approach is recommended.

## Notes on distribution quality

The helper uses SplitMix64-style mixing, which is fast and has good statistical properties for many simulation purposes.
If you need stronger guarantees (e.g., cryptographic properties), use a dedicated library, but keep the same *stateless keyed* structure.
