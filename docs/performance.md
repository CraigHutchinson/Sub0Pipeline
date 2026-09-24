# Performance capture — 2026-09-24

These are no-op host microbenchmarks, not target-device latency bounds or a claim
that the scheduler is universally optimal. Required cancellation/lifetime safety
remains enabled. New measurements replace the old README's unqualified timing
and allocation-free claims.

## Revisions and method

- Baseline: [`d1f5ba0`](https://github.com/CraigHutchinson/Sub0Pipeline/commit/d1f5ba0ce08c8e44c05928b18f1968562a9dcd49), the original cancellation draft.
- Current: [`402dfa0`](https://github.com/CraigHutchinson/Sub0Pipeline/commit/402dfa06b70cabe166d5c8226318024c268ddef6), including correctness fixes and measured-path optimizations.
- Same benchmark source from the current revision compiled against both implementations. The baseline worktree contains that harness-only substitution.
- Linux x86-64, kernel 6.18.44, glibc 2.39; AMD EPYC 9V74 host; 9 logical CPUs exposed to the process.
- GCC 13.3.0, CMake Release, `-O3 -DNDEBUG -std=gnu++23`; no sanitizers or LTO configured.
- Five independent processes per revision, alternating execution order. No concurrent builds or benchmark processes during capture. CPU affinity/frequency were not pinned; this is a shared environment.
- Nanobench: 11 epochs, 1,000 warmup iterations and at least 100,000 iterations per epoch for inexpensive cases. Timeout cases use two warmup iterations and at least ten per epoch.
- Reported value: median of five per-process medians. Bracketed ranges are the minimum and maximum of those five medians, not confidence intervals or worst-case bounds.
- Sequential cases run before timeout cases create native threads. Thread startup, allocator and C++ runtime behavior can differ in an already-multithreaded application.

[Summary JSON](benchmarks/2026-09-24/summary.json) and [raw JSON/text samples](benchmarks/2026-09-24/) retain all epochs, sampling errors and machine output. Timing does not count allocations, stack usage or power consumption.

## Core results

Times below are **nanoseconds per complete operation**, including per-run state
initialization. Repeated-run cases reuse a built graph. First-run validation cost
is not represented by the warm repeated-run figure; explicit validation and
construction are measured separately.

| Workload | Baseline ns [range] | Current ns [range] | Median change |
|---|---:|---:|---:|
| Construct 10-job chain | 957.8 [936.2–1,205.8] | 962.8 [946.3–993.1] | +0.5% |
| Construct 10-job fan-out | 1,098.1 [1,085.7–1,266.8] | 1,123.6 [1,100.6–1,153.0] | +2.3% |
| Run 10-job chain | 377.4 [362.9–436.0] | 350.8 [346.3–368.1] | −7.0% |
| Run 10-job fan-out | 357.8 [352.1–407.2] | 321.3 [309.7–326.3] | −10.2% |
| Run 10-job fan-in | 373.1 [367.5–386.2] | 321.9 [313.4–336.6] | −13.7% |
| Run 4-job diamond | 177.1 [174.3–184.8] | 146.0 [136.9–149.8] | −17.6% |
| Explicitly validate 20-job chain | 100.7 [100.4–103.1] | 105.6 [101.2–107.1] | +4.8% |

The optimizations avoid empty join-lock work and external callback setup on the
no-token path, and cache graph validation/root discovery until a topology edit.
Regression tests cover both dependency-builder directions and cycle detection
after a previously successful run. Fresh cancellation state is still created for
every run. Small construction/validation differences and overlapping ranges do
not establish an intrinsic regression or improvement.

## Opt-in costs

| Feature / workload | Baseline median [range] | Current median [range] | Interpretation |
|---|---:|---:|---|
| Stoppable external token, no request; 10-job chain | 386.3 ns [373.1–448.0] | 543.2 ns [539.9–593.8] | +40.6% versus a baseline that did **not** forward stops to in-flight jobs |
| Counting observer; 10-job chain | 398.2 ns [386.5–432.1] | 370.1 ns [363.0–380.6] | Lightweight start/finish callbacks; real observer work is application-dependent |
| Cooperative 10 ms timeout; one immediately successful job | 10.206 ms [10.185–10.216] | 35.3 µs [23.5–36.9] | Completion now interrupts the watchdog instead of waiting for the deadline |
| Plain 10 ms timeout; one immediate job plus reaping check | 25.6 µs [22.0–38.0] | 34.1 µs [25.2–37.2] | +32.9% median; native-thread variance overlaps substantially, so this does not isolate a stable causal slowdown |

Relative to the current no-token chain, the external forwarding case adds about
192 ns per ten-job run; the counting observer adds about 19 ns. These are separate
no-op measurements, not additive guarantees. The external-token comparison is
not semantically equivalent across revisions: the current implementation fixes
queued/in-flight cancellation gaps. Reverting that safety for speed is not an
acceptable optimization.

Native timeout helpers cost orders of magnitude more than no-op inline dispatch.
They remain strictly opt-in through finite `.timeout()`. A future injected
platform deadline service could avoid per-job threads on constrained devices.
The plain-helper path uses RAII ownership; further optimization requires stable
measurements without weakening joining or exception safety.

## Reproduce and interpret

Follow [CONTRIBUTING.md](../CONTRIBUTING.md), or use the manually triggered
**Performance capture** workflow. The script records raw output and checks case
consistency across all samples. Use the same harness and build flags for both
worktrees, retain source refs, and investigate variance before applying a limit.

Functional correctness, ASan/UBSan, ThreadSanitizer and no-exception core builds
are separate gates. Shared-runner timings are advisory and do not automatically
pass or fail a PR. Memory allocation/resource accounting and real target
measurements remain necessary before claiming fixed-capacity, heap-free,
interrupt-safe or hard real-time behavior.
