# Cancellation and lifetime contract

A generic firmware device validates records, commits durably, then acknowledges
them. A successful commit remains durable if cancellation suppresses the ACK.
Retries require consumer-managed durable identifiers or deduplication; the
scheduler cannot make external side effects idempotent.

## Implemented behavior

- External cancellation reaches executing cooperative jobs through stop callbacks.
  Queued jobs check the live request before body entry, including plain functions.
  A request racing with entry may be observed cooperatively by the body instead.
- Per-job cancellation during a run survives until the job is reached. Pre-run
  requests reset before dispatch. Cancellation is fatal even for optional jobs;
  ordinary optional failures retain their existing behavior.
- `run()` waits for executor callbacks. Non-cooperative timed-out jobs remain
  owned: call `join_orphans()` before releasing borrowed state. Pending queries
  stay true during joins and report unreaped work, not thread liveness.
- Subsequent runs join previous orphans before resetting state. Concurrent or
  reentrant runs return `kBusy`. Do not overlap graph mutation, `arm`, `trigger`,
  moves or destruction with a run. `Job::cancel()` can run concurrently while
  the Pipeline and handles remain valid.
- `trigger()` shares invocation, timeout and status logic with DAG jobs. Wait on
  its executor and join orphans before teardown. It remains an individual job
  invocation, not a token-scoped DAG run.
- Stop callbacks execute synchronously in the requesting thread. Keep them
  bounded; do not join or block on the run owner from a stop callback.
- Cancellation cannot release arbitrary non-cooperative I/O. Transports need
  stop-aware waits or platform cancellation facilities. Joining may be unbounded.

Owner teardown must request stop, join the run thread, and join orphans before
borrowed members are destroyed. Destroying a Pipeline while another thread is
inside `run()` is unsupported. The regression suite tests the explicit owner
shutdown sequence with borrowed state under sanitizers.

## Remaining acceptance work

- Injected deadline service for deterministic timeout tests and threadless targets.
  Current timeout enforcement creates native helper threads, including inline runs.
- Bounded Qt and Zephyr adapter examples with cooperative I/O and shutdown tests.
- If needed, an owned asynchronous run scope whose completion covers jobs,
  queued callbacks and stop registrations, without GUI-thread join deadlocks.

# Embedded extensions: version-next design

## Fixed storage and custom allocators

Audit construction and execution separately. Allocations include node/edge
containers, names, type-erased callables, stop states, queues, validation/skip
scratch, packaged tasks and thread tracking. Standard callable small-object
optimization is not a portable storage guarantee. Stop states and every other
allocation are not automatically covered by a container allocator.

A PMR-backed graph can accept a caller-owned `std::pmr::memory_resource`. A
monotonic buffer with `null_memory_resource()` upstream enforces a graph budget;
the resource must outlive graph, jobs and callbacks. This alone does not provide
heap-free execution. A fixed-capacity execution profile also needs bounded
callable storage, reusable scratch, a chosen cancellation representation, and
executor admission limits. Define exhaustion handling for exception and
no-exception builds; never silently fall back to the global heap.

Validate alignment, resource accounting, exhaustion, repeated runs, graph
capacity, and zero allocations after initialization. Keep a standard dynamic
profile; measure before templating the whole library or adding public policies.

## Interrupt handoff

Scheduler operations (`run`, `trigger`, `cancel`, observers and joining) are
**task-context only**. `request_stop()` may synchronously invoke user callbacks.
An adapter should enqueue stable job/event identifiers into a bounded queue and
wake a task to invoke the scheduler.

Specify SPSC/MPSC and nested-ISR behavior, release/acquire ordering, overflow
(reject, count/drop or coalesce), payload lifetime, generation checks, wakeup-loss
prevention and shutdown draining. Use documented platform ISR primitives or
prove target atomic properties. Lock-free atomics alone do not establish the
whole protocol's interrupt safety.

Current groundwork centralizes execution policy and documents this boundary.
No interrupt API or allocation-free guarantee is added. Validate future adapters
on target toolchains with bounded RAM/stack and no-exception configurations.
