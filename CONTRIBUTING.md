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
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release \
  -DSUB0PIPELINE_BUILD_BENCHMARKS=ON -DSUB0PIPELINE_BUILD_EXAMPLES=OFF
cmake --build build-perf --target Sub0Pipeline_Bench
python3 scripts/capture_benchmarks.py \
  --current build-perf/tests/Sub0Pipeline_Bench --current-ref CURRENT_SHA \
  --baseline /path/to/baseline/Sub0Pipeline_Bench --baseline-ref BASELINE_SHA \
  --features --repeats 5 --output benchmark-results
```

Use two worktrees and copy the current benchmark source into the baseline
worktree when comparing an older implementation; note this harness substitution.
The baseline must support the APIs being measured. The script alternates order,
keeps raw JSON/logs and reports median/min/max of per-process medians. Review
nanobench's within-process errors too. Record CPU, OS, compiler, flags and source
refs. Timing results do not measure memory allocations or worst-case latency.

The manual **Performance capture** workflow produces these artifacts. Shared
runner measurements are advisory; do not enforce a universal nanosecond limit.
Investigate substantial regressions or variance. Explain an intentional cost
with the guarantee it buys. Keep required lifetime safety enabled by default.

## 5. Deliver

Update the root README's feature set, opt-in costs, API descriptions, real
examples and limitations. Update benchmark figures only from retained evidence;
remove stale or unsupported claims. Summarize tests, performance deltas and
unmet acceptance criteria in the PR. Keep incomplete work draft and its issue
open. Do not claim adapter, allocator, interrupt or tracing capabilities until
implemented and validated.
