# Sub0Pipeline

**A lightweight C++23 DAG job scheduler with an expressive operator DSL.**

Build complex dependency graphs in a single expression. Execute them in parallel across embedded and desktop platforms. Zero heap allocations at dispatch time.

```cpp
Pipeline boot;
boot >> "nvs"_job(nvs_init)
     >> "display"_job(display_init).timeout(500ms) + "network"_job(network_init).timeout(500ms)
     >> "app"_job(app_start);
// nvs -> {display || network} -> app
```

---

## Why Sub0Pipeline?

- **One-line DAGs** — Express fan-out, fan-in, diamonds, and layered graphs with `>>` and `+` operators
- **Structured binding capture** — `auto [a, b, c] = pipe >> specs` gives you handles to every job
- **220 ns per 4-job diamond** — Sub-microsecond DAG dispatch overhead
- **Platform-injectable executors** — Same pipeline code runs on ESP32-P4 (FreeRTOS), desktop (std::thread), and bare-metal (sequential)
- **Zero-overhead when you don't need it** — The DSL is opt-in via `SUB0PIPELINE_ENABLE_DSL`. The core API works without it.
- **C++23** — `std::expected` error handling, concepts, structured bindings

Part of the **Sub0** C++ library family.

---

## Quick Start

### Core API

```cpp
#include <sub0pipeline/sub0pipeline.hpp>
using namespace sub0pipeline;

Pipeline boot;

auto nvs     = boot.emplace(nvs_init).name("nvs");
auto display = boot.emplace(display_init).name("display").timeout(500ms);
auto network = boot.emplace(network_init).name("network").timeout(500ms);
auto app     = boot.emplace(app_start).name("app");

display.succeed(nvs);
network.succeed(nvs);
app.succeed(display, network);    // app waits for both

auto exec   = makeDesktopExecutor();
auto result = boot.run(*exec);    // returns std::expected<void, PipelineError>
```

### DSL Extension

```cpp
#define SUB0PIPELINE_ENABLE_DSL 1
#include <sub0pipeline/dsl.hpp>
using namespace sub0pipeline::dsl;

Pipeline boot;
boot >> "nvs"_job(nvs_init)
     >> "display"_job(display_init).timeout(500ms) + "network"_job(network_init).timeout(500ms)
     >> "app"_job(app_start);
```

A single `using namespace sub0pipeline::dsl;` activates everything: operators, the `_job` UDL, and helper types.

---

## DSL Syntax Guide

The DSL uses two operators whose C++ precedence works in your favour:

| Operator | Precedence | Purpose |
|----------|------------|---------|
| `+`      | 6 (tighter) | Group parallel jobs — no dependencies between them |
| `>>`     | 7 (looser)  | Sequence — left side runs before right side |

Because `+` binds tighter than `>>`, **no parentheses are needed** for common patterns:

```cpp
// Linear chain
a >> b >> c >> d;

// Fan-out (a before both b and c)
a >> b + c;                       // parses as: a >> (b + c)

// Fan-in (both a and b before c)
a + b >> c;                       // parses as: (a + b) >> c

// Diamond — one expression
nvs >> display + network >> app;  // nvs -> {display || network} -> app
```

### Named Jobs with `_job` UDL

```cpp
// Create a named job spec, then emplace into a pipeline
auto j = pipe.emplace("init"_job(init_fn).timeout(500ms).optional());

// Structured bindings with multi-emplace
auto [a, b, c] = pipe.emplace(
    "sensor"_job(read_sensor),
    "gps"_job(read_gps).timeout(2s),
    "imu"_job(read_imu)
);
a + b + c >> pipe.emplace("fuse"_job(sensor_fusion));
```

### Inline Pipe Syntax

Build and wire an entire graph in a single expression — no intermediate variables needed:

```cpp
// Fire-and-forget: emplace + wire, no handles stored
pipe >> "A"_job(fn_a) >> "B"_job(fn_b) >> "C"_job(fn_c);

// Unnamed jobs
pipe >> job(fn_a) >> job(fn_b) >> job(fn_c);

// Full inline with fan-out/in
pipe >> "setup"_job(init)
     >> "parse"_job(parse) + "validate"_job(validate)
     >> "commit"_job(commit);
```

### Structured Binding Capture

Capture job handles from parallel groups via `JobTuple`:

```cpp
// Single layer
auto [a, b, c] = pipe >> "A"_job(fn) + "B"_job(fn) + "C"_job(fn);

// Capture + wire to sink
auto [a, b, c] = pipe >> "A"_job(fn) + "B"_job(fn) + "C"_job(fn) >> sink;

// Multi-layer capture via JobTupleChain
auto [l1, l2] = pipe >> "A"_job(fn) + "B"_job(fn) + "C"_job(fn)
                      >> "D"_job(fn) + "E"_job(fn) + "F"_job(fn)
                      >> sink;
auto [a, b, c] = l1;
auto [d, e, f] = l2;
```

### Job Groups with `parallel()`

Group jobs in the core API without the DSL:

```cpp
auto io = parallel(display, network, audio);
io.succeed(nvs);      // all three depend on nvs
app.succeed(io);      // app depends on all three
```

---

## Benchmarks

Measured on Intel Core Ultra 9 275HX, MSVC 1950, Release build, Windows 11.

### DAG Construction

| Benchmark | Time | Throughput |
|-----------|------|------------|
| Construct 10-job linear chain | 2.8 us | 356K ops/s |
| Construct 10-job fan-out | 2.4 us | 408K ops/s |

### Sequential Execution (InlineExecutor)

| Benchmark | Time | Throughput |
|-----------|------|------------|
| 10-job linear chain | 429 ns | 2.3M ops/s |
| 10-job fan-out (1 root + 9 leaves) | 527 ns | 1.9M ops/s |
| 10-job fan-in (9 roots + 1 sink) | 528 ns | 1.9M ops/s |
| 4-job diamond | 220 ns | 4.5M ops/s |

### Validation

| Benchmark | Time | Throughput |
|-----------|------|------------|
| Validate 20-job DAG (Kahn's algorithm) | 265 ns | 3.8M ops/s |

> **Key takeaway:** A 10-job pipeline executes in ~500 ns of scheduler overhead.
> The bottleneck is always your job functions, not the scheduler.

Run benchmarks yourself:
```bash
cmake --preset default -DSUB0PIPELINE_BUILD_BENCHMARKS=ON
cmake --build --preset default
./build/tests/Release/Sub0Pipeline_Bench     # Windows
./build/tests/Sub0Pipeline_Bench             # Linux/macOS
```

---

## Build

```bash
cmake --preset default          # Configure (Release + tests)
cmake --build --preset default  # Build
ctest --preset default          # Run tests
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `SUB0PIPELINE_BUILD_TESTING` | `ON` | Build unit tests |
| `SUB0PIPELINE_BUILD_EXAMPLES` | `OFF` | Build examples |
| `SUB0PIPELINE_BUILD_BENCHMARKS` | `OFF` | Build nanobench benchmarks |
| `SUB0PIPELINE_PLATFORM_DESKTOP` | `ON` | Build `DesktopExecutor` (std::thread) |
| `SUB0PIPELINE_ENABLE_DSL` | `OFF` | Enable operator DSL extension + tests |

---

## CMake Integration

```cmake
# As a subdirectory
add_subdirectory(Sub0Pipeline)
target_link_libraries(MyApp PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Desktop)

# With DSL
target_link_libraries(MyApp PRIVATE Sub0Pipeline::DSL Sub0Pipeline::Desktop)
# SUB0PIPELINE_ENABLE_DSL is defined automatically by the DSL target

# Via find_package (after install)
find_package(Sub0Pipeline REQUIRED)
target_link_libraries(MyApp PRIVATE Sub0Pipeline::Sub0Pipeline)
```

---

## Platform Executors

| Executor | CMake Target | Platform | Description |
|----------|-------------|----------|-------------|
| `SequentialExecutor` | `Sub0Pipeline::Headless` | Any | Inline, no threads. Deterministic. Tests & bare-metal. |
| `DesktopExecutor` | `Sub0Pipeline::Desktop` | Desktop | One `std::thread` per job. Full parallelism. |
| `FreeRtosExecutor` | ESP-IDF component | ESP32-P4 | `xTaskCreatePinnedToCore`. Dual-core. |

See [PLATFORM_ROADMAP.md](PLATFORM_ROADMAP.md) for planned executors.

---

## API Reference

### Pipeline

| Method | Description |
|--------|-------------|
| `emplace(fn)` | Add a job; returns a `Job` handle |
| `emplace(spec)` | Add a job from a `JobSpec` (DSL) |
| `emplace(specs...)` | Multi-emplace; returns `std::tuple<Job...>` for structured bindings |
| `run(executor, observer?)` | Execute DAG; returns `std::expected<void, PipelineError>` |
| `validate()` | Check for cycles (Kahn's algorithm; called automatically by `run()`) |
| `status(job)` / `name(job)` | Query job state and name |
| `add_tick(tick)` | Register a recurring tick job for the post-boot loop |
| `run_loop()` | Enter the post-boot event loop (`[[noreturn]]`) |
| `dump_text()` | Print DAG structure to stdout |

### Job (fluent builder)

```cpp
job.name("nvs")
   .timeout(5s)
   .core(1)           // CPU affinity (-1 = any)
   .stack(8192)        // FreeRTOS stack bytes
   .priority(10)       // FreeRTOS priority (1-24)
   .optional()         // failure won't block dependents
   .succeed(other)     // this job runs after other
   .precede(other)     // other runs after this job
```

### DSL Types

| Type | Description |
|------|-------------|
| `JobSpec<F>` | Named job descriptor with builder methods. Created by `"name"_job(fn)` or `job(fn)`. |
| `JobSpecGroup<Fs...>` | Deferred parallel group. Created by `spec + spec`. |
| `JobTuple<N>` | Fixed-size job group with structured binding support. Returned by `pipe >> specs`. |
| `JobTupleChain<Layers...>` | Multi-layer accumulator. Returned by `tuple >> specs`. |
| `JobGroup` | Runtime-sized job group. Created by `parallel()` or `job + job`. |

### Error Handling

Jobs return `std::expected<void, PipelineError>`. Void-returning lambdas are auto-wrapped as always-succeeding.

```cpp
auto job = pipe.emplace([]() -> std::expected<void, PipelineError> {
    if (failed) return std::unexpected(PipelineError::kJobFailed);
    return {};
});
```

| Error | Meaning |
|-------|---------|
| `kTimeout` | Job exceeded its declared timeout |
| `kJobFailed` | Job function returned an error |
| `kDependencyFailed` | A required predecessor failed |
| `kCyclicDependency` | DAG contains a cycle |
| `kDuplicateJob` | Job added more than once |
| `kUnknownJob` | Operation on invalid handle |

---

## Examples

| Example | Description |
|---------|-------------|
| [`minimal_pipeline`](examples/minimal_pipeline/) | Linear A -> B -> C chain |
| [`boot_sequence`](examples/boot_sequence/) | Parallel boot: nvs -> {display &#124;&#124; network} -> app |
| [`parallel_tasks`](examples/parallel_tasks/) | Fan-out + fan-in with atomic counters |
| [`dsl_operators`](examples/dsl_operators/) | DSL syntax: inline pipe, structured bindings, `_job` UDL |
| [`error_handling`](examples/error_handling/) | Required/optional failures, propagation |
| [`validate_dag`](examples/validate_dag/) | Cycle detection, `dump_text()` |
| [`observer_profiling`](examples/observer_profiling/) | Custom `IObserver` with progress bar |
| [`job_options`](examples/job_options/) | Every builder method demonstrated |
| [`on_demand_jobs`](examples/on_demand_jobs/) | Event-triggered jobs (stub) |
| [`tick_loop`](examples/tick_loop/) | Post-boot recurring tasks |

---

## Project Structure

```
include/sub0pipeline/
  sub0pipeline.hpp          Public API (Pipeline, Job, JobGroup, IExecutor, IObserver)
  dsl.hpp                   DSL extension (operators, _job UDL, JobSpec, JobTuple)
src/
  sub0pipeline.cpp          DAG engine implementation
platform/
  desktop/                  std::thread executor
  headless/                 Inline sequential executor
  esp32p4/                  FreeRTOS executor
tests/
  test_pipeline.cpp         Core DAG + JobGroup + parallel() tests
  test_dsl.cpp              DSL operators, _job UDL, JobTuple, JobTupleChain tests
  test_failure.cpp          Error propagation tests
  test_validation.cpp       Cycle detection tests
  test_observer.cpp         Observer hook tests
  test_concurrent.cpp       Thread-safety tests
  bench_pipeline.cpp        nanobench performance benchmarks
examples/
  10 worked examples (see table above)
```

---

## Sister Libraries

- **[Sub0Pub](https://github.com/CraigHutchinson/Sub0Pub)** — Zero-overhead typed publish-subscribe

---

## License

MIT — see [LICENSE.md](LICENSE.md)
