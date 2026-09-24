# Sub0Pipeline

A product-agnostic **C++23 DAG job scheduler** with explicit dependencies,
pluggable executors, cancellation and an optional operator DSL. Use it for device
initialization, finite data-processing batches and ordered application work.

Define the graph once, then run it in dependency order. Independent jobs can
execute concurrently when the selected executor supports it. The library uses
`std::expected` for job results and standard C++ ownership/cancellation facilities.
Jobs and observers should not throw across executor callbacks; report job failures
through `std::expected`.

## Feature set

| Capability | Implemented behavior |
|---|---|
| Dependency graphs | Chains, fan-out, fan-in, diamonds and runtime-sized graphs; `succeed()` / `precede()` and `JobGroup` |
| Graph validation and reuse | Cycle validation before the first run and after topology edits; cached roots/validation on unchanged graphs; fresh per-run cancellation state |
| Optional DSL | Separate `dsl.hpp`; `>>`, `+`, `_job`, job groups, tuples and structured bindings |
| Job results | `std::expected<void, PipelineError>`; void jobs are wrapped as successful jobs |
| Failure propagation | Required failures skip successors; optional ordinary failures allow them to continue; cancellation remains fatal |
| Cancellation | Per-job `cancel()` and external stop tokens for `run`, `run_inline` and `run_until`; queued plain jobs are also suppressible |
| Timeouts | Native helpers or an injected deadline service with caller-owned registrations; expiry reports `kTimeout`; owned plain workers require reaping |
| Structured completion | Opt-in `RunScope` requests stop and joins executor callbacks, deadline callbacks and orphan workers before teardown |
| Completion and reuse | `run()` waits for executor callbacks; `join_orphans()` waits for timed-out helper jobs; subsequent runs reap old work; concurrent/reentrant runs return `kBusy` |
| Executors | Inline/headless, desktop thread-per-job, priority worker pool, scoped adapter, and an ESP32-P4 FreeRTOS source adapter |
| Job hints | Names, status text, timeout, priority, core affinity and stack size; platform hints depend on the executor |
| Observation and diagnostics | Start/finish/failure hooks, status/name queries, snapshots, first failure name, error context and text DAG dump |
| On-demand jobs | `add_on_demand`, `arm`, `trigger`; excluded from normal roots and invoked individually |
| Repeated work | Stop-controlled DAG reruns with `run_until`; periodic ticks with `add_tick` / `run_loop` |
| Build and validation | CMake targets/install support, optional executor builds, no-exception core configuration, examples, functional/sanitizer suites and opt-in benchmarks |

**Not current guarantees:** allocation-free execution, custom graph allocators,
ISR-safe scheduling, hard real-time deadlines, forced interruption of arbitrary
I/O, work stealing, distributed jobs, or automatic idempotency of external writes.
`dump_trace()` is a stub and the dependency observer hook is not currently emitted.
Bounded Qt/Zephyr examples and injected deadline services are available; hardware
validation and fixed-capacity execution remain separate work. See
[embedded and cancellation design notes](docs/structured-cancellation.md).

## Quick start

```cpp
#include <sub0pipeline/sub0pipeline.hpp>
using namespace sub0pipeline;

Pipeline pipe;
auto decode = pipe.emplace([] { /* decode a device record */ });
auto validate = pipe.emplace([] { /* validate decoded fields */ });
auto commit = pipe.emplace([]() -> std::expected<void, PipelineError> {
    // Persist the validated record. Return unexpected(...) on failure.
    return {};
});
validate.succeed(decode);
commit.succeed(validate);

auto result = pipe.run_inline();
```

Choose an executor explicitly for parallel work:

```cpp
auto executor = makePriorityExecutor(2); // two workers; queue is dynamically sized
// Keep the executor, pipeline and anything borrowed by jobs alive through joining.
auto result = pipe.run(*executor);
pipe.join_orphans();
```

### Optional DSL

```cpp
#include <sub0pipeline/dsl.hpp>
using namespace sub0pipeline;
using namespace sub0pipeline::dsl;

Pipeline pipe;
pipe >> "load"_job([] {})
    >> ("parse"_job([] {}) + "check"_job([] {}))
    >> "commit"_job([] {});
// load -> {parse, check} -> commit
```

Use `JobSpec`, `JobSpecGroup`, `JobTuple`, `JobTupleChain` and `JobGroup` to compose
larger graphs. See the [DSL example](examples/dsl_operators/main.cpp) for supported
composition and structured-binding patterns. The DSL adds no separate scheduler;
it builds the same graph as the core API.

## Cancellation, deadlines and lifetimes

```cpp
std::stop_source shutdown;
Pipeline pipe;
auto transfer = pipe.emplace([](std::stop_token stop)
    -> std::expected<void, PipelineError> {
    if (stop.stop_requested())
        return std::unexpected(PipelineError::kCancelled);
    // Use stop-aware transport waits for real blocking I/O.
    return {};
});
auto result = pipe.run_inline(shutdown.get_token());
pipe.join_orphans(); // before destroying anything borrowed by a timed-out job
```

An external request reaches executing cooperative jobs through their token.
Pending jobs check cancellation before body entry, even without a token
parameter. A request racing with entry may arrive after the body has started.
Per-job cancellation before a run resets during initialization; requests during
the run survive until the job is reached.

`run()` returning and **all borrowed state being safe to release are distinct**
when non-cooperative jobs time out. `has_pending_orphans()` reports unreaped work,
including threads being joined; it is not a replacement for synchronization.
`join_orphans()` can block indefinitely if a job never returns. The next run joins
old timed-out work before reinitializing. Untimed inline jobs stay on the calling
thread; timeout enforcement can create native helper threads even inline.

Owner teardown must request stop, join its run thread, then join orphans **before
borrowed members are destroyed**. Destroying a Pipeline concurrently with `run()`
is unsupported. Stop callbacks run synchronously in the requesting thread; they
must not block on the run they are cancelling. Avoid overlapping graph edits,
`arm`, `trigger`, moves or destruction with an active run.

For a device's validate → durable commit → ACK sequence, failed required commit
suppresses ACK. Cancellation can suppress ACK after commit succeeds; the write
is not rolled back. The consumer must supply durable deduplication/idempotent
retry behavior. See the [complete contract](docs/structured-cancellation.md).

## Costs and opt-in behavior

| Path or feature | Selection | Cost / limit |
|---|---|---|
| Core DAG execution | Always | Job state, dependency counters, cancellation checks, run guard and fresh stop states; dynamic allocations remain |
| Validation | Automatic on topology change; explicit `validate()` available | Traverses the graph and allocates scratch; cached for unchanged repeated runs |
| External cancellation forwarding | Supply a stoppable token | Stop callback registration per executing job; skipped for the no-token path |
| Observer callbacks | Supply an `IObserver*` | Virtual calls and user callback work; absent when no observer is supplied; callbacks can run concurrently |
| Timeout enforcement | Set a finite `.timeout()` | Native helpers by default; injected cooperative deadlines avoid helper threads; plain bodies still use a worker |
| Owned run thread | Construct `RunScope` | One native run thread plus stop state; completion joins callbacks and orphan workers |
| Timeout reaping | A plain timed job exceeds its deadline | Thread tracking and join; empty registry avoids join-lock work |
| Priority worker pool | Select `makePriorityExecutor(n)` | Fixed worker count, dynamic priority queue; no queue-capacity/backpressure guarantee |
| Desktop execution | Select `makeDesktopExecutor()` | One native thread per dispatched job |
| Snapshots / text diagnostics | Call the API | Snapshot allocation or formatting/I/O; not automatic |
| DSL | Include `dsl.hpp` | Compile-time composition; ordinary graph-construction costs still apply |
| Benchmarks | Build option + manual execution/CI | Not part of library execution; expensive timeout samples require `--features` |

Correctness and lifetime guarantees are not disabled to improve benchmark scores.
Fixed/custom storage and interrupt handoff need additional platform contracts;
reserving graph containers alone would not eliminate allocations from callables,
stop states, scratch or executor queues.

## Executors and use-case boundaries

| Executor | Target / location | Behavior |
|---|---|---|
| `SequentialExecutor` | `Sub0Pipeline::Headless` | Executes inline; deterministic untimed test scheduling |
| `DesktopExecutor` | `Sub0Pipeline::Desktop` | Thread per job; joins dispatched work; ignores priority/affinity hints |
| `PriorityExecutor` | `Sub0Pipeline::Priority` | Configurable worker count; higher priorities start first; already-running work is not preempted |
| `ScopedExecutor` | Core header | Reuses a parent executor and waits only for locally dispatched jobs |
| `QtExecutor` | `examples/qt_bounded` | Private bounded QThreadPool; caller-runs overflow; no GUI event-loop dependency |
| `ZephyrExecutor` | `examples/zephyr_bounded` | Fixed slots and one Zephyr worker; caller-runs overflow; validated on native simulator |
| `FreeRtosExecutor` | `platform/esp32p4` | ESP-IDF/FreeRTOS source adapter with task priority, affinity and stack hints; not validated by host CI |

`ScopedExecutor` supports inner pipelines without waiting for the outer job's
own completion count. It does **not** solve worker starvation: if all workers in
a bounded pool synchronously wait for inner work, no worker remains to run it.
Use inline inner work, reserved capacity or a separate execution resource.

`trigger()` accepts an individual on-demand invocation; observe completion or
wait on the armed executor, then reap timeout jobs. Concurrent submissions need
a thread-safe executor, stable graph and thread-safe job/observer state. There
is no per-invocation future. Duplicate queued/running submissions return `kBusy`;
wait for executor completion before retrying. Each accepted invocation receives
a fresh cancellation state. Foreign handles are rejected.

`run_until()` reruns a graph until stopped; the caller supplies pacing.
`run_loop()` is a blocking tick loop with approximately millisecond polling and
no stop-token overload. These are not hard real-time scheduling guarantees.
Scheduler APIs are **task-context only**. ISR integration should enqueue a bounded
event for later task-context dispatch using platform-proven primitives.

## Build and integration

CMake 3.21+ and a C++23 standard library with `std::expected` are required.

```sh
cmake --preset ci-unix
cmake --build --preset ci-unix
ctest --preset ci-unix --timeout 30
```

Use `ci-msvc` on Windows. The `default` preset also enables benchmarks; the CMake
option itself defaults to OFF.

```cmake
add_subdirectory(Sub0Pipeline)
target_link_libraries(MyDevice PRIVATE Sub0Pipeline::Sub0Pipeline)
# Add Sub0Pipeline::Headless, ::Desktop or ::Priority when using its factory.
```

Installed builds also support `find_package(Sub0Pipeline REQUIRED)`.

| CMake option | Default | Purpose |
|---|---|---|
| `SUB0PIPELINE_BUILD_TESTING` | `ON` | Functional and DSL tests |
| `SUB0PIPELINE_BUILD_EXAMPLES` | `OFF` | Available source examples |
| `SUB0PIPELINE_BUILD_BENCHMARKS` | `OFF` | Nanobench executable; requires test build enabled |
| `SUB0PIPELINE_PLATFORM_DESKTOP` | `ON` | Desktop executor target |
| `SUB0PIPELINE_PLATFORM_PRIORITY` | `ON` | Priority executor target |
| `SUB0PIPELINE_EXCEPTIONS` | `ON` | Library hard-error throwing; OFF supports a core `-fno-exceptions` build |

Tests exercise all host executors and require their targets. A lean consumer
build can disable testing, examples, benchmarks and unused executor targets.
No-exception configuration does not by itself bound memory or make allocation
exhaustion recoverable.

## API map

| Area | Entry points |
|---|---|
| Construct / connect | `emplace`, `emplace_void`, `succeed`, `precede`, `parallel`, `size` |
| Execute / cancel | `run`, `run_inline`, `run_until`, `Job::cancel` |
| Join / inspect | `join_orphans`, `has_pending_orphans`, `status`, `name`, `snapshot` |
| Diagnose | `validate`, `first_failure_name`, `set_current_job_error`, `dump_text` |
| Events / ticks | `add_on_demand`, `arm`, `trigger`, `add_tick`, `run_loop` |
| Job configuration | `name`, `status`, `timeout`, `optional`, `priority`, `core`, `stack` |

Job statuses distinguish pending, ready, running, done, failed, skipped, timed
out and cancelled. Errors include `kJobFailed`, `kTimeout`, `kCancelled`,
`kCyclicDependency`, `kUnknownJob`, `kNotArmed`, `kNotOnDemand` and `kBusy`.
`kDependencyFailed` and `kDuplicateJob` are also declared error values; they are
not a promise of additional runtime duplicate/dependency diagnostics.

`IObserver` provides dispatch start, finish/progress and failure hooks. With
parallel executors, callbacks may overlap and must protect shared state.
A dispatch-start notification does not prove that a cancelled job body ran.
See the [public header](include/sub0pipeline/sub0pipeline.hpp) for signatures.

## Injected deadlines and owned runs

```cpp
#include <sub0pipeline/deadline.hpp>
#include <sub0pipeline/run_scope.hpp>

// service, executor, graph and borrowed state must outlive the run scope.
pipeline.set_deadline_service(&service); // IDeadlineService, configured while idle
sub0pipeline::RunScope run{pipeline, executor};
run.request_stop();                     // a request, not completion
const auto result = run.join();         // callbacks + timed-out workers joined
```

`IDeadlineService` arms a caller-owned `Deadline` for the job's execution duration.
It can use a platform timer or a manually advanced clock and fixed registration
slots. `cancel_and_wait` must unregister and drain expiry callbacks; exhaustion
returns `kDeadlineUnavailable` without running the body. Expire from task context,
because stop callbacks execute synchronously. See [the contract and coverage](docs/structured-cancellation.md).

Cooperative expiry reports `kTimeout` after the body returns, even if the body
ignores its stop token and returns success. It cannot forcibly interrupt arbitrary
I/O. Untimed jobs do not consult the service. Plain timed bodies still require a
native worker; no heap-free or globally threadless execution claim is implied.
`RunScope::complete()` becomes true after joining all owned work; destruction
requests stop and joins. Do not join from jobs or callbacks needed by that run.

## Performance evidence

See [measured baseline/current results](docs/performance.md), including machine,
compiler, source revisions, five alternating process samples, observed ranges
and the costs of optional features. Figures describe no-op host workloads, not
application throughput, worst-case latency or target-device guarantees.

```sh
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release \
  -DSUB0PIPELINE_BUILD_BENCHMARKS=ON -DSUB0PIPELINE_BUILD_EXAMPLES=OFF
cmake --build build-perf --target Sub0Pipeline_Bench
./build-perf/tests/Sub0Pipeline_Bench --json results.json --features
```

The benchmark build reuses the vendored nanobench dependency. The capture script
and manually triggered CI workflow retain machine-readable evidence. Follow
[CONTRIBUTING.md](CONTRIBUTING.md) for comparisons and regression review.

## Examples, tests and contribution

| Example | Demonstrates |
|---|---|
| [minimal_pipeline](examples/minimal_pipeline/main.cpp) | Linear dependencies |
| [boot_sequence](examples/boot_sequence/main.cpp) | Initialization fan-out/fan-in |
| [parallel_tasks](examples/parallel_tasks/main.cpp) | Desktop parallel workers and aggregation |
| [on_demand_jobs](examples/on_demand_jobs/main.cpp) | Armed event jobs |
| [dsl_operators](examples/dsl_operators/main.cpp) | DSL composition and structured bindings |

The suites include core and DSL regression tests plus optional Qt and Zephyr
adapter executions. Coverage includes
repeat runs, topology-cache invalidation, failure/cancellation edges, queued and
cooperative work, owner teardown, scoped execution and worker completion. Release,
ASan/UBSan and ThreadSanitizer are separate validation configurations; performance
runs use unsanitized Release binaries.

[AGENTS.md](AGENTS.md), [CONTRIBUTING.md](CONTRIBUTING.md) and the PR template codify
C++23/reuse, product-agnostic wording, ownership, embedded constraints, performance
evidence and documentation gates. Remaining cancellation/adapter work is tracked
in [issue #1](https://github.com/CraigHutchinson/Sub0Pipeline/issues/1).
