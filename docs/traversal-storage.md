# Reusable validation and failure traversal storage

Continuation of issue #4. This change targets traversal scratch while retaining
the dynamic standard-library profile and fresh cancellation state for every run.

Validation now keeps its indegree array and ready list in the Pipeline. A mutex
serializes simultaneous validation queries; graph mutation must still be idle.
Each node enters the ready list at most once. Capacity is retained across queries
and resized when the graph grows. Automatic validation remains cached after a
successful run until topology changes.

Failure propagation uses one retained worklist per Pipeline and one active drainer
per run. Workers claim a node's skipped status before queueing it, so shared
successors appear at most once and the logical list cannot exceed the node count.
The first failure reserves storage before status changes; subsequent failure runs
reuse it. Successful runs do not reserve failure scratch. Observer callbacks run
outside the traversal lock. The drainer stays inside an executor callback until
all queued skip notifications have returned, preserving the completion boundary.
A queued job must claim Ready → Running and cannot overwrite a skipped state.

This trades retained capacity and validation-query serialization for fewer
allocations. It does not impose an application-selected byte limit or change
allocation-exhaustion behavior. Vectors may retain more capacity than their logical
size. This is not a fixed/custom allocator API, a heap-free scheduler, or proof
of bounded platform stack/latency. Native helpers, callables and stop states retain
their existing allocation/lifetime contracts.

## Allocation evidence

Three independent GCC 13.3/libstdc++ Release captures agreed exactly; raw CSV is
in [allocations/2026-09-25-traversal](allocations/2026-09-25-traversal/). Compare
with the [prior audit](allocation-audit.md), which documents the measurement
window, successful C++ new calls and requested bytes, and what it cannot count.

| Operation | Before calls / bytes | After calls / bytes |
|---|---:|---:|
| First run, 10-job chain | 14 / 860 | 13 / 324 |
| Warm run, 10-job chain | 10 / 240 | 10 / 240 |
| Repeated explicit validation, 10-job chain | 3 / 616 | 0 / 0 |
| Warm required failure + skipped successor | 4 / 624 | 2 / 48 |
| Construct/destroy 10-job chain | 26 / 4,912 | 26 / 4,976 |

Cancellation allocations remain deliberately intact. Reusing a stopped source
would allow tokens retained from an earlier run to interfere with later runs.
A future cancellation-storage profile needs its own explicit token and exhaustion
contract; graph-only allocator support cannot solve this.

## Correctness and performance gates

Regression tests cover growing/cyclic graphs, simultaneous validation queries,
concurrent failures with shared descendants, exactly-once skip notifications,
blocked observer completion and reuse after failure. Existing cancellation and
rerun tests remain in force. The core also compiles with exceptions disabled.

Performance capture compares the merged allocation-audit baseline with this
change using the same harness, Release flags and host. Results and full validation
status are recorded in the PR and accompanying benchmark evidence.

## Same-host performance capture

Baseline: `fc6467d30c01a5ab1bd4681f6b5bdb0a4c9a9c89`; current code:
`d5fbbbd8f5002c6d82c5d064af0b6eb175e78a1d`. GCC 13.3/libstdc++,
Linux 6.18.44 x86-64, AMD EPYC 9V74, nine available logical CPUs. Both
binaries used the same unchanged harness and `-std=c++23 -O3 -DNDEBUG -pthread`.
No sanitizer/LTO, affinity or frequency pinning. Five alternating independent
process samples with `--features`; no concurrent local builds or benchmark runs.

Times are ns per complete operation: median of five process medians with observed
min/max. They are not confidence intervals, target-device bounds or causal proof.

| Operation | Baseline ns [min–max] | Current ns [min–max] | Median change |
|---|---:|---:|---:|
| construct 10-job linear chain | 967.4 [953.9–1,016.4] | 995.6 [988.6–1,006.6] | +2.9% |
| construct 10-job fan-out (1 root + 9 leaves) | 1,098.8 [1,091.8–1,156.8] | 1,109.0 [1,081.8–1,122.4] | +0.9% |
| 10-job linear chain | 352.5 [338.1–375.8] | 335.4 [329.0–348.4] | -4.8% |
| 10-job fan-out (1 root + 9 leaves) | 316.5 [295.8–321.4] | 299.8 [295.9–322.0] | -5.3% |
| 10-job fan-in (9 roots + 1 sink) | 307.6 [292.2–312.7] | 295.5 [287.8–316.7] | -3.9% |
| 4-job diamond | 133.0 [130.2–158.0] | 132.9 [129.2–135.3] | -0.1% |
| validate 20-job chain | 102.4 [100.8–111.7] | 75.4 [74.8–80.1] | -26.4% |
| external stoppable token, no request | 577.7 [563.1–618.5] | 550.1 [545.9–564.4] | -4.8% |
| observer callbacks, no external token | 369.7 [365.2–373.5] | 357.0 [349.9–376.2] | -3.5% |
| cooperative timeout configured | 27,140.0 [22,139.4–35,917.0] | 26,622.8 [22,750.2–39,069.0] | -1.9% |
| plain timeout configured plus join | 25,871.0 [20,588.9–35,495.8] | 25,454.9 [22,261.2–35,078.7] | -1.6% |

[Raw samples and summary](benchmarks/2026-09-25-traversal/) retain all measurements.
The summary was verified against the raw sample arrays. Explicit validation's
median improved about 26% on this host; default DAG cases showed no median
regression in this capture. Small construction increases have overlapping ranges.
Native helper timing is noisy; no universal speed claim is inferred. Failure-path
latency, memory high-water marks and real-target behavior are not measured here.
