# Committed output (GVT-safe side effects)

Time Warp is optimistic: an LP may process an event, emit downstream events, and later roll back.
That makes **side effects** (printing, logging to a file, writing metrics, talking to external systems) tricky:
if you do them directly inside `on_event()`, a rollback can cause duplicates or out-of-order output.

WarpSim’s recommended pattern is **committed output**:

- During event execution you *record* an output payload.
- The kernel *emits* it only once it is irrevocable, i.e. when the event’s timestamp is strictly less than $\mathrm{GVT}$.

## API

### 1) Produce output in model code

Use the event context:

```cpp
void on_event(const warpsim::Event& ev, warpsim::IEventContext& ctx) override {
    warpsim::Payload p;
    p.kind = 1234;
    p.bytes = warpsim::bytes_from_trivially_copyable(MyOutput{...});

    // Tie committed output to the current event timestamp.
    ctx.emit_committed(ev.ts, std::move(p));
}
```

Notes:
- `emit_committed()` is rollback-safe: if the event is undone, the recorded output is undone too.
- The kernel currently requires `ts == ev.ts` for clarity.

### 2) Consume output in the simulation host

Provide a sink in `SimulationConfig`:

```cpp
warpsim::SimulationConfig cfg;
cfg.committedSink = [&](warpsim::RankId rank, warpsim::LPId lp, warpsim::TimeStamp ts, const warpsim::Payload& p) {
    // safe to print/log/write externally here
};
```

The sink is invoked by the kernel in **deterministic order**, sorted by:

1. timestamp
2. LPId
3. originating event src
4. originating event uid
5. per-event output index

## What this is (and isn’t)

- This is not a special “logging system”. It’s a general mechanism for *any* irreversible side effect.
- Normal internal simulation behavior (sending events, updating state) should still happen inside `on_event()`.
- For rollback-safe randomness, see [random.md](random.md).
