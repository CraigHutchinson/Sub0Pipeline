# Sub0Pipeline

**A lightweight C++23 DAG job scheduler with an expressive operator DSL.**

Build complex dependency graphs in a single expression. Execute them in parallel with pluggable backends. Sub-microsecond scheduler overhead.

```cpp
Pipeline pipe;
pipe >> "load"_job(load_data)
     >> "parse"_job(parse).timeout(500ms) + "validate"_job(validate).timeout(500ms)
     >> "commit"_job(commit);
// load -> {parse || validate} -> commit
```

---

## Why Sub0Pipeline?

- **One-line DAGs** — Express fan-out, fan-in, diamonds, and layered graphs with `>>` and `+` operators
- **Structured binding capture** — `auto [a, b, c] = pipe >> specs` gives you handles to every job
- **220 ns per 4-job diamond** — Sub-microsecond DAG dispatch overhead
- **Platform-injectable executors** — Same pipeline code runs with std::thread, sequential/inline, or custom RTOS backends
- **Zero-overhead when you don't need it** — The DSL lives in its own namespace and header. Just `#include <sub0pipeline/dsl.hpp>` to use it.
- **C++23** — `std::expected` error handling, concepts, structured bindings

Part of the **Sub0** C++ library family.

---

## Use-case fit

A quick reference for where Sub0Pipeline is the right tool and where it is not.

**Fit key:** ✅ Strong &nbsp; 🟡 Partial &nbsp; ❌ Weak / not the right tool

### Initialization and sequencing

| Use case | Fit | Notes |
|---|---|---|
| Boot / init sequence with dependency ordering | ✅ | The primary use case -- `auth >> load_config >> {mount || tcp} >> ready` |
| Staged shutdown (reverse-ordered teardown) | ✅ | Reverse the edges; `run()` handles the ordering |
| Feature flag / canary rollout stages | ✅ | One pipeline per stage; `optional()` for non-blocking gates |
| Plugin / module loading with inter-dependencies | ✅ | Each plugin is a job; `succeed()` encodes load order |

### Batch and data processing

| Use case | Fit | Notes |
|---|---|---|
| Single-shot ETL / data pipeline (ingest → transform → validate → store) | ✅ | Natural fit; fan-out for parallel transform, fan-in for aggregate |
| Asset import pipeline (per-batch: dedup → fetch → cache → notify) | ✅ | Finite DAG per batch; `IObserver` provides per-job progress telemetry |
| Build systems (compile → link → test → package) | ✅ | Dependency graph maps directly; `validate()` catches cycles at construction |
| Fan-out / scatter-gather parallelism | ✅ | `a >> b + c + d >> sink`; first-class in the DSL |
| Map-reduce (N workers → aggregate) | ✅ | Fan-out N jobs, fan-in to one; structured bindings give handles to all N |
| Processing items of unknown count at runtime | 🟡 | `emplace()` works at runtime before `run()`; DAG is immutable once execution starts |

### Event-driven and I/O

| Use case | Fit | Notes |
|---|---|---|
| Hardware / ISR / network event dispatch (fire-and-forget) | 🟡 | `add_on_demand()` + `arm()` + `trigger()` -- dispatches immediately; no result awaiting |
| One pipeline per request (HTTP handler, RPC call) | ✅ | Construct a small pipeline per request; `SequentialExecutor` keeps it stack-local |
| Streaming / continuous I/O (audio decode, video encode, network receive loop) | 🟡 | `run_until(exec, stop_token)` re-runs the DAG in a loop; pacing is the caller's responsibility |
| Producer-consumer queues (bounded, multi-consumer) | 🟡 | `add_on_demand()` + `trigger()` dispatches a drainer job on each enqueue; no built-in queue storage |

### Long-running services and daemons

| Use case | Fit | Notes |
|---|---|---|
| Periodic tick tasks at fixed intervals | 🟡 | `add_tick()` + `run_loop()` -- 1 ms granularity; no sub-millisecond cadence |
| Daemon startup then event loop | ✅ | `run()` for the startup DAG, `run_loop()` for steady-state ticks, `trigger()` for events |
| Perpetual background worker threads | 🟡 | `run_until(exec, stop_token)` loops `run()` until stopped; combine with `std::jthread` for lifecycle |
| Work-stealing thread pools (unbounded runtime queue) | ❌ | DAG topology is defined before execution; not a general task queue |

### Real-time and embedded

| Use case | Fit | Notes |
|---|---|---|
| RTOS task scheduling (FreeRTOS, ESP32) | 🟡 | `FreeRtosExecutor` available; `core()` and `priority()` map to `xTaskCreatePinnedToCore`; no preemption |
| Fixed-rate game / simulation update (update → physics → render) | 🟡 | DAG-per-frame works; overhead is 220--500 ns for small graphs; re-run support confirmed |
| Hard real-time deadlines (< 10 µs response) | 🟡 | `SequentialExecutor` gives deterministic sub-microsecond dispatch; `DesktopExecutor` has OS scheduling jitter |
| ISR-safe, zero-allocation dispatch | 🟡 | Allocations happen at construction (`emplace`), not at dispatch; `run()` itself is allocation-free after the DAG is built |

### Concurrency patterns

| Use case | Fit | Notes |
|---|---|---|
| Fork-join parallelism | ✅ | `executor.wait_all()` at the end of `run()`; all forks joined before return |
| Diamond dependency (A → {B ∥ C} → D) | ✅ | Core benchmark topology; 220 ns overhead |
| Priority-ordered execution | 🟡 | `priority()` hint passed to executor; enforcement depends on the executor backend |
| Cross-process job distribution | ❌ | Single-process only; no serialization, no remote dispatch |
| Dynamic graph modification after execution starts | ❌ | DAG structure is immutable once `run()` is called; build the full graph before running |

### Testing and determinism

| Use case | Fit | Notes |
|---|---|---|
| Unit-testable pipelines (swap executor for determinism) | ✅ | `SequentialExecutor` runs jobs inline, in order; no threads, no non-determinism |
| Reproducible execution order verification | ✅ | `RecordingExecutor` pattern (see tests) captures dispatch order before execution |
| Timeout injection in tests | ✅ | `.timeout()` per job; `kTimeout` error propagates like any other failure |

---

## Quick Start

### Core API

```cpp
#include <sub0pipeline/sub0pipeline.hpp>
using namespace sub0pipeline;

Pipeline pipe;

auto load     = pipe.emplace(load_data).name("load");
auto parse    = pipe.emplace(parse_input).name("parse").timeout(500ms);
auto validate = pipe.emplace(validate_input).name("validate").timeout(500ms);
auto commit   = pipe.emplace(commit_result).name("commit");

parse.succeed(load);
validate.succeed(load);
commit.succeed(parse, validate);    // commit waits for both

auto exec   = makeDesktopExecutor();
auto result = pipe.run(*exec);      // returns std::expected<void, PipelineError>
```

### DSL Extension

```cpp
#include <sub0pipeline/dsl.hpp>
using namespace sub0pipeline::dsl;

Pipeline pipe;
pipe >> "load"_job(load_data)
     >> "parse"_job(parse_input).timeout(500ms) + "validate"_job(validate_input).timeout(500ms)
     >> "commit"_job(commit_result);
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

### Test Coverage

119 test cases across two suites:

| Suite | Focus | Cases |
|-------|-------|-------|
| `Sub0Pipeline_Tests` | Core DAG engine, JobGroup, re-runnability, error propagation, on-demand jobs | 84 |
| `Sub0Pipeline_DslTests` | Operators, `_job` UDL, JobTuple, JobTupleChain, inline pipe | 35 |

Tested topologies include linear chains, fan-out/in, diamonds, double-diamonds, W-shapes, hourglasses, binary trees (31 nodes), and stress tests (100+ nodes). Error propagation covers required/optional failure cascading, mid-graph failures, multi-root failure ordering, and full recovery on re-run. The epoch-based re-runnability is verified across all topologies with counter, ordering, and status assertions.

---

## CMake Integration

```cmake
# As a subdirectory
add_subdirectory(Sub0Pipeline)
target_link_libraries(MyApp PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Desktop)

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
| `add_tick(tick)` | Register a recurring tick job for the event loop |
| `run_loop()` | Enter the tick event loop (`[[noreturn]]`) |
| `run_until(executor, stop_token)` | Re-run the pipeline in a loop until the stop token is signalled |
| `arm(executor, observer?)` | Store executor for later `trigger()` calls |
| `add_on_demand(fn)` | Register a job excluded from `run()`; dispatched only via `trigger()` |
| `trigger(job)` | Dispatch an on-demand job via the armed executor; returns `std::expected` |
| `dump_text()` | Print DAG structure to stdout |

### Job (fluent builder)

```cpp
job.name("task")
   .timeout(5s)
   .core(1)           // CPU affinity (-1 = any)
   .stack(8192)        // executor stack bytes
   .priority(10)       // executor priority (1-24)
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
| `kNotArmed` | `trigger()` called before `arm()` |
| `kNotOnDemand` | `trigger()` called on a regular (non-on-demand) job |

---

## Examples

| Example | Description |
|---------|-------------|
| [`minimal_pipeline`](examples/minimal_pipeline/) | Linear A -> B -> C chain |
| [`boot_sequence`](examples/boot_sequence/) | Parallel fan-out/in: A -> {B &#124;&#124; C} -> D |
| [`parallel_tasks`](examples/parallel_tasks/) | Fan-out + fan-in with atomic counters |
| [`dsl_operators`](examples/dsl_operators/) | DSL syntax: inline pipe, structured bindings, `_job` UDL |
| [`error_handling`](examples/error_handling/) | Required/optional failures, propagation |
| [`validate_dag`](examples/validate_dag/) | Cycle detection, `dump_text()` |
| [`observer_profiling`](examples/observer_profiling/) | Custom `IObserver` with progress bar |
| [`job_options`](examples/job_options/) | Every builder method demonstrated |
| [`on_demand_jobs`](examples/on_demand_jobs/) | Event-triggered jobs: `arm()` + `trigger()` with atomic counters |
| [`tick_loop`](examples/tick_loop/) | Recurring tick tasks after pipeline completion |

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
