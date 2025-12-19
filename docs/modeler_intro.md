# Modeler introduction

This doc is a quick, practical starting point for writing models on WarpSim.

## Mental model

- You write a set of **Logical Processes (LPs)** that own state.
- LPs communicate by sending timestamped events.
- The kernel runs optimistically and can **roll back** LPs when late events arrive.

The most important takeaway:

- Model code must be deterministic and rollback-safe.

Start with [modeling_contract.md](modeling_contract.md).

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
