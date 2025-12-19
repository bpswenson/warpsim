# Logging

WarpSim includes a small built-in logger in `src/log.hpp`.

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

## When to use an external library

If you want structured logging, async sinks, file rotation, or formatting, `spdlog` is a good next step.
The built-in logger is deliberately tiny and dependency-free.
