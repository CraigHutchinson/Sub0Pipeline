# Contribution workflow

Read [AGENTS.md](AGENTS.md). Source, issues, PRs and examples describe a generic
firmware device or other generic workload; the library remains product-agnostic.

## 1. Define the change

State the observable problem, ownership contract and intended compatibility.
List costs added to the default path versus opt-in paths. For embedded changes,
consider runtime allocation, caller-owned resources, bounded queues, stack use,
no-exception behavior and task/interrupt context. Do not add speculative policy
layers just to name a future capability.

## 2. Implement and review

Use C++23 standard facilities and RAII; reuse the existing execution path and
platform adapters. Check cancellation at body entry and DAG edges, optional
failure semantics, queued callbacks, teardown and repeated runs. Document
non-cooperative I/O limits. Cache invariants only when every topology mutation
invalidates them. Keep borrowed objects alive until their final callback ends.

## 3. Validate correctness

```sh
cmake --preset ci-unix
cmake --build --preset ci-unix
ctest --preset ci-unix --timeout 30
cmake --preset ci-asan
cmake --build --preset ci-asan
ctest --preset ci-asan --timeout 30
cmake --preset ci-tsan
cmake --build --preset ci-tsan
ctest --preset ci-tsan --timeout 30
```

Use the Windows preset on MSVC. Lifetime/concurrency changes require relevant
sanitizer evidence; explain unsupported environments. A local LeakSanitizer
restriction may require `ASAN_OPTIONS=detect_leaks=0`; record it and keep the
normal CI leak checks enabled. Add behavior-based tests using queued executors,
latches and injected clocks rather than assumptions about wall-clock scheduling.

For core embedded changes, also build with `SUB0PIPELINE_EXCEPTIONS=OFF` and
`-fno-exceptions` on a supporting compiler. A desktop no-exception build is not
proof of RTOS or bare-metal compatibility. Target adapters need target builds.

## 4. Capture performance

Build Release without sanitizers. Use the same compiler, flags, benchmark source
and machine for both revisions. Separate graph construction, steady-state run,
explicit validation, external cancellation, observer and timeout/executor costs.
Do not run benchmark processes concurrently or under unrelated build load.

```sh
cmake --preset perf-unix          # perf-msvc on Windows
cmake --build --preset perf-unix --target Sub0Pipeline_Bench
python3 scripts/capture_benchmarks.py \
  --current build-perf/tests/Sub0Pipeline_Bench --current-ref CURRENT_SHA \
  --baseline /path/to/baseline/Sub0Pipeline_Bench --baseline-ref BASELINE_SHA \
  --features --repeats 5 --output benchmark-results
```

The `perf-*` presets are Release code generation plus debug symbols, so the
same binary serves timing and profiling. `Sub0Pipeline_Bench --list` names the
cases; `--case SUBSTR` runs a subset while iterating.

Use two worktrees and copy the current benchmark source into the baseline
worktree when comparing an older implementation; note this harness substitution.
A baseline older than the harness may also need `Sub0Pipeline::Priority` linked
into its benchmark target. Cases one side lacks are listed under
`cases_without_counterpart` and left out of the comparison.
The baseline must support the APIs being measured. The script alternates order,
keeps raw JSON/logs and reports median/min/max of per-process medians. Review
nanobench's within-process errors too. Record CPU, OS, compiler, flags and source
refs. Timing results do not measure memory allocations or worst-case latency.

The manual **Performance capture** workflow produces these artifacts. Shared
runner measurements are advisory; do not enforce a universal nanosecond limit.
Investigate substantial regressions or variance. Explain an intentional cost
with the guarantee it buys. Keep required lifetime safety enabled by default.

### Profile before changing code

A timing says that a case is slow, not why. Before optimizing, attribute the
time with a profiler and state the hypothesis the change tests:

```sh
python3 scripts/profile_vtune.py --bench build-perf/tests/Sub0Pipeline_Bench \
  --ref CURRENT_SHA --case "10-job linear chain" --output profile-results
python3 scripts/profile_vtune.py --bench build-perf/tests/Sub0Pipeline_Bench \
  --ref CURRENT_SHA --analysis threading \
  --case "priority(4): 300-job fan-out" --output threading-results
```

`hotspots` ranks functions by CPU time and `threading` by time spent waiting on
locks and condition variables; neither needs elevation. Each case runs alone
in its own process for a fixed wall time. `profile.md` lists functions twice:
with inlined callees folded in (what to change) and by inlined frame (which
operation costs the time). Pass `--keep-results` to open a result in the VTune
GUI. Profiled runs carry instrumentation overhead: use them for attribution
only and take every timing from `capture_benchmarks.py`. Intel VTune is the
supported profiler; on other hosts use an equivalent sampling profiler against
`Sub0Pipeline_Bench --exact --case NAME --profile-seconds N` and record the
tool and version.

The loop for each optimization is: capture a baseline, profile the case, change
one thing, re-capture against that baseline, re-profile to confirm the hot spot
moved, and re-run the allocation audit. Keep a change only when the timing
moves outside the baseline's observed range or the allocation counts drop; a
profile alone is not evidence of a speedup. See
[the performance audit](docs/performance-audit.md) for a worked example.

For allocation-sensitive work, also run `Sub0Pipeline_AllocationAudit` from the
benchmark build in three independent processes. Retain CSV, compiler/library and
source revision; distinguish requested bytes from live/peak memory and C++ `new`
from platform/allocator calls it cannot observe. Do not time instrumented runs.
See [the audit method and limits](docs/allocation-audit.md). These counts supplement,
not replace, fixed-budget exhaustion and real-target tests.

## 5. Deliver

Routine PR updates run the GCC build and tests. Before merging, request the full
platform, sanitizer and adapter suite by adding the `ci:full` label, transitioning
a draft to ready for review, or dispatching **CI** manually on the PR branch.
The label is an event trigger: later commits run the quick check even if the
label remains. Remove and re-add it to validate a newer head. Accept only a full
suite that passed on the exact head being merged. Main/develop pushes and merge
queue entries also run the full suite; superseded PR runs are cancelled.

Update the root README's feature set, opt-in costs, API descriptions, real
examples and limitations. Update benchmark figures only from retained evidence;
remove stale or unsupported claims. Summarize tests, performance deltas and
unmet acceptance criteria in the PR. Keep incomplete work draft and its issue
open. Do not claim adapter, allocator, interrupt or tracing capabilities until
implemented and validated.
