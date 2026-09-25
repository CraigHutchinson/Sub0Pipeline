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
