# WarpSim / warpdes

WarpSim (library name: `warpdes`) is an optimistic parallel discrete-event simulation (PDES) engine written in C++20.

Core features:
- **Time Warp** optimistic execution with rollback + anti-messages
- **Global Virtual Time (GVT)** computation and fossil collection
- Optional **MPI transport** for multi-process runs
- **Deterministic event UIDs** (tied to LPId, not MPI rank)
- **Committed output**: GVT-safe side effects emitted exactly once
- **State snapshots** for rollback: LP-wide `save_state/load_state` and optional per-entity `save_entity/load_entity` (triggered by `ctx.request_write(entityId)`)
- Optional higher-level modeling patterns (e.g., AOI LP pattern) and examples

This repo builds a library (`warpdes`) plus unit tests and examples under `tests/` and `examples/`.

## Correctness-first (current priority)

Right now, **correctness and determinism are prioritized over speed**.

In particular, we aim for behavior that is stable under different MPI sizes and different LP→rank assignments (e.g., the same model parameters should produce the same committed-output summaries when run at `-n 3` vs `-n 20`). Long-term, the goal is performance and speedup from additional ranks, but it has to be correct first.

## Project origin / disclaimer

This project started as a paired-programming experiment with ChatGPT (GPT-5.2) over winter break 2025 to evaluate code generation capabilities.

We are doing our best to ensure everything works as intended, but please treat the codebase accordingly: review changes carefully, run the tests for your environment, and validate correctness for your model/use case.

## Build

### Non-MPI build

```bash
cmake -S . -B build-nompi -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWARPSIM_ENABLE_MPI_TESTS=OFF \
  -DWARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS=ON
cmake --build build-nompi -j
ctest --test-dir build-nompi --output-on-failure
```

### MPI build

```bash
cmake -S . -B build-smoke -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWARPSIM_ENABLE_MPI_TESTS=ON \
  -DWARPSIM_ENABLE_EXAMPLE_SMOKE_TESTS=ON
cmake --build build-smoke -j
ctest --test-dir build-smoke --output-on-failure
```

## Determinism across MPI ranks (example)

The Fujimoto-style airport example emits a deterministic, GVT-safe committed-output stream and prints a deterministic summary:

```
airport_fujimoto hash=<H> arrivals=<A> departures=<D> maxQ=<Q>
```

This is intended to be **rank-count independent**: running with the same parameters should produce the same `hash` and counters whether you run without MPI, with MPI `-n 3`, or with MPI `-n 20`.

### Example: default parameters

Build with MPI enabled (see above), then run:

```bash
./build-smoke/examples/warpdes_airport_fujimoto
mpiexec -n 3 ./build-smoke/examples/warpdes_airport_fujimoto
mpiexec --oversubscribe -n 20 ./build-smoke/examples/warpdes_airport_fujimoto
```

Observed output (all three invocations):

```
airport_fujimoto hash=6614527102907929045 arrivals=199985 departures=196098 maxQ=252
```

Notes:
- Some OpenMPI installs require `--oversubscribe` when `-n 20` exceeds available slots/cores.

### Example: CTest-registered deterministic configuration

There is also a smaller configuration used by CTest (fast, deterministic, exercises committed output):

```bash
./build-smoke/examples/warpdes_airport_fujimoto \
  --airports 8 --planes 64 --end 400 --service-max 5 --flight-max 9 --seed 1 --emit-plane-mod 4

mpiexec -n 3 ./build-smoke/examples/warpdes_airport_fujimoto \
  --airports 8 --planes 64 --end 400 --service-max 5 --flight-max 9 --seed 1 --emit-plane-mod 4

mpiexec --oversubscribe -n 20 ./build-smoke/examples/warpdes_airport_fujimoto \
  --airports 8 --planes 64 --end 400 --service-max 5 --flight-max 9 --seed 1 --emit-plane-mod 4
```

Observed output (all three invocations):

```
airport_fujimoto hash=4209842363547979520 arrivals=808 departures=754 maxQ=17
```

To run the corresponding tests:

```bash
ctest --test-dir build-smoke --output-on-failure -R 'warpdes\\.example_airport_fujimoto|warpdes\\.mpi_airport_fujimoto'
```

Observed test results:

```
100% tests passed, 0 tests failed out of 3

Start 17: warpdes.example_airport_fujimoto
Start 19: warpdes.mpi_airport_fujimoto_3ranks
Start 20: warpdes.mpi_airport_fujimoto_20ranks
```

## Docs

Project docs live in `docs/`:
- [docs/modeler_intro.md](docs/modeler_intro.md)
- [docs/modeling_contract.md](docs/modeling_contract.md)
- [docs/integrating_with_cmake.md](docs/integrating_with_cmake.md)
- [docs/state_store.md](docs/state_store.md)
- [docs/committed_output.md](docs/committed_output.md)
- [docs/random.md](docs/random.md)
- [docs/aoi.md](docs/aoi.md)
- [docs/airplanes_missile_aoi_demo.md](docs/airplanes_missile_aoi_demo.md)

## Examples

Common entry points:
- `examples/warpdes_airport_fujimoto` (determinism + committed output)
- `examples/warpdes_demo`
- `examples/warpdes_market_demo`
- `examples/warpdes_radar_explosion_demo`
- `examples/warpdes_phold_mpi` (MPI benchmark-style)
