# Modeler introduction

This doc is a quick, practical starting point for writing models on WarpSim.

## Mental model

- You have **things you want to model** (customers, aircraft, packets, orders, vehicles, …).
- Those things are represented as **entities** identified by an `EntityId`.
- The simulator executes your model by running **Logical Processes (LPs)** that process timestamped events.
- The kernel runs optimistically and can **roll back** when late events arrive.

The most important takeaway:

- Model code must be deterministic and rollback-safe.

Start with [modeling_contract.md](modeling_contract.md).

If you want more detail on how rollback snapshots relate to entities and `ctx.request_write(...)`, see [state_store.md](state_store.md).

## Quick glossary (practical)

WarpSim has a few core concepts; model code reads much easier once these click:

- **Entity**: “your thing”. A piece of model state addressed by `EntityId`. If migration is enabled, ownership of an entity can move.
- **LP (Logical Process)**: an execution container that runs `on_event(...)`. LPs are how the simulator partitions work; they are also the unit of rollback.
- **Event**: a timestamped message that causes some state change.

If you only care about modeling entities, that’s fine — you can write models where LPs mostly act as routers/executors and the interesting state lives in entities.

## LP vs entity (and migration)

It’s easy to get confused because both LPs and entities can “own state”. The difference is:

- LP state is *always* local to that LP.
- Entity state can be treated as the “real model state”, and (optionally) migrated between LPs.

### Addressing: send to an LP vs send to an entity

Events support both styles:

- **LP-addressed**: set `ev.dst` (and usually keep `ev.target = 0`). Use this when the destination LP is stable by design.
- **Entity-targeted (migration-safe)**: set `ev.target = entityId`. When directory routing / ownership tracking is enabled, the kernel routes the event to the correct owner-at-time (the right `dst` can change as the entity migrates).

Tip:

- Prefer `modeling::send_targeted(...)` / `modeling::make_targeted_event(...)` when your “destination” is really an entity that might migrate.

See [migration.md](migration.md) for a full walkthrough (directory routing, policy API, and verification).

## Minimal skeleton

An LP looks like:

```cpp
struct MyState { /* ... */ };

class MyLP final : public warpsim::ILogicalProcess {
public:
  explicit MyLP(warpsim::LPId id) : m_id(id) {}
  warpsim::LPId id() const noexcept override { return m_id; }

  void on_start(warpsim::IEventSink& sink) override {
    // seed initial work (optional)
  }

  void on_event(const warpsim::Event& ev, warpsim::IEventContext& ctx) override {
    // 1) request snapshot before mutating state
    // ctx.request_write(...);
    // 2) update state
    // 3) send future events via ctx.send(...)
    // 4) for irreversible side effects, use ctx.emit_committed(...)
  }

  warpsim::ByteBuffer save_state() const override { /* serialize */ }
  void load_state(std::span<const std::byte> bytes) override { /* deserialize */ }

private:
  warpsim::LPId m_id;
  MyState m_state;
};
```

This skeleton shows **LP-local state** (`MyState`). That’s a good fit when your “thing” is naturally per-LP.

If your “thing” is better represented as an **entity** (and especially if you want migration), treat the LP as an executor and keep the interesting state in entities:

```cpp
// Sketch only: entity state lives in a World keyed by EntityId.
// The LP processes events and reads/writes entity state.

class MyLP final : public warpsim::ILogicalProcess {
public:
  explicit MyLP(warpsim::LPId id, warpsim::World& world) : m_id(id), m_world(world) {}
  warpsim::LPId id() const noexcept override { return m_id; }

  void on_event(const warpsim::Event& ev, warpsim::IEventContext& ctx) override {
    if (ev.target != 0) {
      // Read/write the addressed entity. (Writes must request snapshots.)
      warpsim::WorldTxn txn(m_world, ctx);
      // auto& state = txn.write<MyEntityState>(ev.target, MyEntityType);
      // ... mutate state ...
    }
  }

private:
  warpsim::LPId m_id;
  warpsim::World& m_world;
};
```

## Creating and running a simulation

At the host level:

- Create a `SimulationConfig`
- Add LP instances via `sim.add_lp(...)`
- Seed initial events via `on_start()` or `sim.send()`
- Call `sim.run()` (or `sim.run_one()` for stepping demos)

See the examples:

- `warpdes_demo` (migration + directory routing)
- `warpdes_radar_explosion_demo` (AOI pattern)
- `warpdes_airport_fujimoto` (airport/queueing model; demonstrates committed output and rank-count independence)

## Side effects and printing

If you print inside `on_event()`, rollbacks can duplicate output.

Use committed output instead:

- produce with `ctx.emit_committed(ev.ts, payload)`
- consume with `SimulationConfig::committedSink`

See [committed_output.md](committed_output.md).

## Randomness

Do not use global mutable RNGs.
Use deterministic keys as described in [random.md](random.md).

## Recommended helper header

For most models, include [src/modeling.hpp](../src/modeling.hpp) and use its helpers to
reduce boilerplate and make mistakes fail fast:

- Typed payloads: `modeling::pack()` / `modeling::unpack()`
- Typed events: `modeling::make_event()` / `modeling::send()` / `modeling::require()`
- Targeted (migrating) events: `modeling::make_targeted_event()` / `modeling::send_targeted()`
- Rollback-safe mutation: `modeling::write(ctx, entityId)`
- Rollback-safe RNG: `modeling::rng_u64()` / `modeling::rng_unit_double()` / `modeling::rng_u64_range()`
