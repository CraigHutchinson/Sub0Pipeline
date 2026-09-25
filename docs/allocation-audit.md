# C++ allocation audit — 2026-09-24

This opt-in executable replaces global C++ `new`/`delete` **only inside the audit
process**. It counts successful allocation calls and requested bytes during a
measurement window, including calls on owned helper threads. It is not linked
into the scheduler or applications. A calibration checks ordinary and aligned
allocation interception before collecting results.

## Scope and method

GCC 13.3/libstdc++, Linux 6.18.44 x86-64, C++23 Release (`-O3 -DNDEBUG -pthread`),
without sanitizers. Scheduler source matches merged PR #3 (`a3f1b20`). Three
independent process runs produced identical counts; [raw CSV](allocations/2026-09-24/)
is retained. Counts below are normalized per operation; CSV records totals and
operation counts. Construction and first execution are separate from warmed runs.
The audit source is `tests/audit_allocations.cpp` in this revision.

The window excludes reporting, prior graph construction and executor setup unless
that case explicitly includes them. Every window joins its owned work before
ending. The native-thread cases run after the inexpensive sequential cases, so
process/runtime initialization can affect their results.

This measures **C++ allocation requests**, not total heap usage, live/peak memory,
allocator metadata, resident memory, stacks, `malloc` calls, memory-mapped regions
or platform-driver allocations. Shared-library interception and compiler allocation
elision are toolchain-dependent. A zero count would not establish heap-free
execution. Counting atomics perturb timing; do not use this executable for latency.

## Observed counts per operation

| Operation | C++ allocation calls | Requested bytes |
|---|---:|---:|
| Construct and destroy 10-job chain | 26 | 4,912 |
| First run of existing 10-job chain | 14 | 860 |
| Warm run of 10-job chain | 10 | 240 |
| Warm chain with external stoppable token | 10 | 240 |
| Explicit validation of 10-job chain | 3 | 616 |
| Snapshot of 10 jobs | 1 | 240 |
| Warm required failure and successor skip | 4 | 624 |
| One cooperative job, injected no-expiry service | 1 | 24 |
| One cooperative job, native watchdog | 4 | 144 |
| Owned run scope around 10-job chain | 13 | 328 |
| On-demand invocation and join | 2 | 48 |

The warmed-chain result is consistent with the current fresh-per-run stop-state
construction. Registering the external callback adds no allocation calls in this
fixture; it still has synchronization cost. The injected fixture isolates the
core registration path: it deliberately never expires and allocates no timer
storage. It does not represent a real timer driver's memory budget. Behavior at
expiry is covered separately by the deadline regression tests.

## Implications for fixed/custom storage

Graph-only PMR support would leave execution allocations in stop states, failure
traversal scratch, on-demand callback storage and optional native helper/run
threads. Do not introduce it as an allocation-free profile. A useful next design
must specify separate graph, execution and executor budgets, with no silent global
heap fallback when a fixed budget is exhausted.

Prioritize cancellation storage and reusable traversal scratch before promising
zero-allocation warmed execution. Preserve per-run token freshness: a token retained
from a completed run must never cancel a later run. Fixed-capacity APIs also need
admission/exhaustion results, callback storage limits, alignment tests, bounded
stack use and no-exception behavior. Standard `stop_source` does not accept a
caller allocator; changing its representation would be an API/lifetime design
change, not a container substitution.

## Reproduce

```sh
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release \
  -DSUB0PIPELINE_BUILD_BENCHMARKS=ON -DSUB0PIPELINE_BUILD_EXAMPLES=OFF
cmake --build build-perf --target Sub0Pipeline_AllocationAudit
build-perf/tests/Sub0Pipeline_AllocationAudit > allocations.csv
```

The audit requires exceptions for standard allocation-failure behavior. It is
omitted when `SUB0PIPELINE_EXCEPTIONS=OFF`; the library's no-exception build is
unchanged. Local capture used the equivalent direct GCC command because CMake
was unavailable in the resumed workspace:

```sh
g++ -std=c++23 -O3 -DNDEBUG -pthread -Iinclude \
  tests/audit_allocations.cpp src/sub0pipeline.cpp -o allocation-audit
./allocation-audit
```

The manual Performance capture workflow builds the audit and retains three CSV
runs alongside timing evidence. Timing still uses five alternating baseline/current
samples; deterministic allocation counts use three process runs to check stability.

## Follow-up

[Reusable traversal storage](traversal-storage.md) updates validation and failure
scratch after this baseline capture. Original measurements above remain retained
as historical evidence; use the follow-up report for the changed paths.
