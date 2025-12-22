# Logging

WarpSim includes a small built-in logger in `src/log.hpp`.

If you need rollback-safe printing/metrics/file output from model code, also see [committed_output.md](committed_output.md).

## Enable

Set `SimulationConfig::logLevel`:

- `LogLevel::Off` (default)
- `LogLevel::Error`
- `LogLevel::Warn`
- `LogLevel::Info`
- `LogLevel::Debug`
- `LogLevel::Trace`

Example:

```cpp
warpsim::SimulationConfig cfg;
cfg.rank = 0;
cfg.logLevel = warpsim::LogLevel::Debug;
warpsim::Simulation sim(cfg, transport);
```

## What it prints

Log lines are prefixed with:

- level
- rank
- LP id
- simulation timestamp (time.sequence)

This is intended for debugging kernel behavior (rollbacks, anti-cancels, etc.).

Note: direct logging from inside an LP’s `on_event()` can be replayed during rollback.
For “print this once” style output, use committed output.

## Migration visibility

To verify that migrations are happening when your policy says they should:

- Prefer the rollback-safe committed-output migration records documented in [migration.md](migration.md).
- If you want to confirm that targeted events are being routed via a directory LP, set `SimulationConfig::logLevel = LogLevel::Trace` and watch for "route targeted event" log lines.

## When to use an external library

If you want structured logging, async sinks, file rotation, or formatting, `spdlog` is a good next step.
The built-in logger is deliberately tiny and dependency-free.
