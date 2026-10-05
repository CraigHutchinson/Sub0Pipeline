# Performance audit — 2026-10-05

A profile-led review of where Sub0Pipeline spends time, with ranked candidates
for optimization. Nothing here changes library behavior: this records the
starting point and the method, so each candidate can be taken through the
measure, profile, change, re-measure loop in [CONTRIBUTING.md](../CONTRIBUTING.md).

These are no-op-job host microbenchmarks. They measure scheduling overhead, not
target-device latency, and say nothing about pipelines whose jobs do real work.

## Method

- Library at [`7a373d2`](https://github.com/CraigHutchinson/Sub0Pipeline/commit/7a373d2);
  benchmark harness, presets and scripts from the change that adds this document.
- Windows 11, Intel Core Ultra 9 275HX (24 logical CPUs, 8 P + 16 E cores).
  MSVC 19.51, `perf-msvc` preset: `/O2 /Ob2 /DNDEBUG /Zi`, no LTO.
- Timing: `scripts/capture_benchmarks.py --features --repeats 5`. Values are the
  median of five per-process medians; brackets are the minimum and maximum of
  those five, not confidence intervals.
- Attribution: `scripts/profile_vtune.py`, Intel VTune 2026.4, 10 s per case, one
  case per process. `hotspots` (user-mode sampling) for CPU time and `threading`
  for lock and condition-variable waits.
- Allocations: `Sub0Pipeline_AllocationAudit`, three processes, identical output.
- No affinity or frequency pinning. An IDE was open and the machine sat at
  roughly 8–11% background CPU, so treat differences of a few percent as noise.

Evidence: [timing](benchmarks/2026-10-05-audit/timing/summary.json),
[hotspots](benchmarks/2026-10-05-audit/hotspots/profile.md),
[threading](benchmarks/2026-10-05-audit/threading/profile.md) and the raw samples
beside them. Two results below (the fan-out limit and the recursion depth) come
from a one-off probe program and are described so they can be reproduced.

## Timings

| Case | Median [range] | Per job |
|---|---:|---:|
| Construct 10-job chain | 1.93 µs [1.82–2.17] | 193 ns |
| Construct 1000-job layered DAG | 152.5 µs [148.2–167.2] | 153 ns |
| Run 10-job chain, inline | 691 ns [678–699] | 69 ns |
| Run 200-job chain, inline | 14.5 µs [14.1–14.7] | 72 ns |
| Run 300-job fan-out, inline | 20.5 µs [20.0–20.9] | 68 ns |
| Run 1000-job layered DAG, inline | 80.1 µs [78.2–80.5] | 80 ns |
| Validate 1000-job layered DAG | 5.56 µs [5.23–5.66] | 5.6 ns |
| Snapshot 1000-job layered DAG | 4.94 µs [4.82–5.24] | 4.9 ns |
| Run 10-job chain, external stop token | 1.06 µs [1.04–1.08] | 106 ns |
| Run 10-job chain, counting observer | 771 ns [750–781] | 77 ns |
| Run 10-job chain, `PriorityExecutor(4)` | 16.1 µs [14.6–17.6] | 1.6 µs |
| Run 1000-job layered DAG, `PriorityExecutor(4)` | 557 µs [488–571] | 557 ns |
| Run 300-job fan-out, `PriorityExecutor(4)` | 214 µs [199–241] | 714 ns |
| On-demand trigger and wait, `PriorityExecutor(4)` | 5.09 µs [3.84–8.55] | — |
| Run 10-job fan-out, `DesktopExecutor` | 322 µs [300–350] | 32 µs |
| One job with a cooperative 10 ms timeout | 47.8 µs [38.1–53.3] | — |
| One job with a plain 10 ms timeout, plus join | 56.9 µs [48.2–70.6] | — |

Inline scheduling cost is flat at about 70–80 ns per job from 10 to 1000 jobs.
Handing no-op jobs to a four-thread pool costs seven to ten times more per job
than running them inline, because each hand-off is a lock and a thread wake-up.

## Findings

Ranked by how much they matter, correctness first.

### 1. A job cannot have more than about 360 successors

Building one root with 400 dependents throws `succPool_ exceeded 65535-entry
uint16_t address range` (or terminates in a no-exception build). A successor
list that outgrows its four inline slots moves to a shared pool, and every
later append copies the whole list to a fresh block and abandons the old one.
A list of *n* successors therefore consumes about *n*²/2 pool entries, and the
pool's 16-bit index is exhausted at roughly 362. The limit is per pipeline, so
several wide jobs lower it further.

The same copying makes construction quadratic: the probe measured 154 ns per
leaf at 50 leaves and 749 ns per leaf at 360.

The comment on `PoolSuccessors::push_back` describes the waste as "typically
≤ 35 abandoned entries per node"; that is not what the code does. Growing the
block geometrically (the unused inline slots can hold a capacity once a list is
in pool mode) would make pool use linear. The pool's 16-bit addressing would
still cap the overflowed edges of one pipeline at some tens of thousands, well
short of what the documented 65,536-job graph size suggests. This is a defect,
not only a slow path, and it is why the benchmark's wide fan-out case stops at
300 jobs.

### 2. Inline execution recurses once per dependency link

With an inline executor (`run_inline()`, `SequentialExecutor`), a completing job
dispatches its successor from inside its own call, so stack depth grows with
the longest dependency chain. A probe built with Clang `-O2` ran an 1,800-job
chain and overflowed the 1 MB main stack at 2,000, which is about 0.5 KB per
link. On an 8 KB embedded task stack that budget is a chain of roughly fifteen
jobs. The README presents the sequential executor as the bare-metal option, so
this deserves either a documented limit or a queue-and-drain loop in the inline
executors. A drain loop would change sibling execution order from depth-first
to breadth-first, which tests that rely on deterministic order would notice.

### 3. Fresh cancellation state is about half the cost of a small run

`run()` gives every node a new `std::stop_source` before dispatch. In the
10-job chain profile, by inlined frame:

| Operation | Share of CPU |
|---|---:|
| `std::stop_source::operator=` (release old state, install new) | 26% |
| `malloc` (the new stop state) | 17% |
| `std::stop_source::get_token` and `std::stop_token` destructor | 13% |
| Job status compare-and-swap | 15% |
| `dispatchJob` itself | 12% |

The allocation audit agrees: a warm 10-job run makes 10 allocations of 32
bytes, one per node, and nothing else. The share is similar at 200 and 300 jobs
and falls to about 47% on the 1000-job layered graph.

The fresh state exists so that a token retained from one run can never observe
a later run's cancellation. That guarantee only matters for jobs that receive
the token. A job added through the plain `emplace([] {...})` overloads is
wrapped so that it never sees one, so its stop state could be reused across
runs unless a stop was actually requested on it, and it could be handed an
empty token. Cancellable jobs would keep today's behavior. Expected effect: the
plain-job cases lose their per-node allocation and most of the 56% above; this
needs measuring, and it must not weaken the retained-token guarantee
([issue #4](https://github.com/CraigHutchinson/Sub0Pipeline/issues/4)).

### 4. Forwarding an external stop token costs 53% on a 10-job run

1,055 ns against 691 ns. Each job registers its own `std::stop_callback` on the
external token for the duration of its body. One callback per run that requests
stop on every node would make the no-request path constant instead of per-job.
Pending jobs already check their own stop state before running, so cancellation
semantics should be unchanged; that needs the cancellation tests and
ThreadSanitizer to confirm.

### 5. `PriorityExecutor` is bounded by one queue lock and a wake-up per job

In the threading profile of the 1000-job layered graph, 97% of wait time is
acquiring the queue mutex: 51% in `dispatch` and 46% in the worker loop. CPU
time tells the same story from the other side: `condition_variable_any::wait`
and mutex acquisition account for most samples, which on user-mode sampling is
the cost of putting workers to sleep and waking them. Candidates, each to be
measured separately:

- Take the completion lock only when the in-flight count reaches zero, not on
  every job.
- Wait on a plain `std::condition_variable` with a stop flag; the stop-token
  overload of `condition_variable_any` registers a callback on every wait.
- Let a worker that dispatches successors keep one for itself instead of
  queueing it and waking a peer.

For no-op jobs the pool will stay slower than inline execution whatever is done
here. The useful target is the per-job hand-off cost, currently 0.6–1.6 µs.

### 6. Construction: about 2.8 allocations and one vector regrowth chain per job

`malloc` is 39% of the 10-job construction profile and moving and destroying
`Node` objects during `std::vector` regrowth is another 21%. The allocation
audit counts 28 allocations for a 10-job pipeline. Two contributors are
avoidable:

- Each plain job is stored as a `std::function` wrapped in a lambda inside a
  second `std::function`, so it allocates twice and is called through two
  indirections on every run (the `_Func_class::operator()` rows, 8–11% of run
  time). Building the stored callable directly from the user's callable in the
  template `emplace` overload would remove one of each.
- There is no way to reserve node storage. A `reserve(jobCount)` call would
  remove regrowth for callers that know their graph size. That is new public
  API and belongs with the bounded-allocation work in issue #4.

Every job is also given a default name (`"job_N"`) that `.name()` then replaces;
that is about 9% of construction and could be made lazy.

### 7. `DesktopExecutor` costs about 32 µs per job

It creates a thread per job by design, and the threading profile shows the run
thread waiting in `std::thread::join`. This is documented behavior and not a
defect. User-mode sampling attributes almost nothing to its short-lived
threads, so `hotspots` is the wrong tool for this executor; use `threading`.

### Not a problem

- Validation is 5.6 ns per node on a warm graph and is cached between runs.
- Observer callbacks add about 8 ns per job for a counting observer.
- Per-job inline cost rises only from 69 ns to 80 ns between a 10-job and a
  1000-job graph, although `Node` is 168 bytes and the larger graph no longer
  fits in L1. Splitting hot and cold node fields is not worth doing yet.
- Timeout helpers cost tens of microseconds because they start a thread. That
  is opt-in, and the injected deadline service already avoids it for
  cooperative jobs.

## Suggested order

1. Fix successor pool growth (finding 1). It is a correctness limit; the win in
   construction time is a side effect.
2. Reuse stop state for jobs that never receive a token (finding 3). Largest
   expected gain on the default path, and the retained-token guarantee has to be
   argued carefully.
3. One external-stop callback per run (finding 4).
4. `PriorityExecutor` hand-off cost (finding 5), one candidate at a time.
5. Single-wrap job storage (finding 6), then `reserve` with issue #4.
6. Decide between documenting and removing the inline recursion (finding 2).

## Limits of this audit

- One host, one compiler, one operating system. GCC and Clang inline and
  allocate differently; `std::stop_source`, `std::function` and
  `condition_variable_any` are standard-library implementations, and their
  costs here are MSVC's.
- User-mode sampling folds kernel time into the calling user function and
  cannot say how much of a wait is the kernel. Hardware event sampling
  (`uarch-exploration`) needs elevation and was not run.
- No real-target measurement. Nothing here establishes behavior on an RTOS.
- The benchmark's pool size is fixed at four workers; scaling with worker
  count was not measured.
