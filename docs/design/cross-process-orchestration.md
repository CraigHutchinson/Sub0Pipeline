# Cross-process orchestration: deferred completion and remote sub-graphs

Status: deferred completion is implemented in the core library. Remote
sub-graphs are a proposal for a separate optional target; nothing in this
document's "Remote sub-graphs" part is implemented yet.

This document is the single source of truth for both features. Each design
element names the requirement it satisfies with a stable handle in backticks,
for example `req-deferred-completion`. The requirement list is at the end.

## Problem

A job completes when its body returns. A job whose real work happens elsewhere
-- in another process, on a GPU queue, in an asynchronous I/O completion or
behind a timer -- must therefore block an executor thread for the whole wait.
With a bounded pool of N workers, N such jobs stall every CPU job in the graph.

Two features fix this, in two layers:

1. **Deferred completion (core).** A job body starts work and returns. The job
   completes when a completion handle is signalled, from any thread. No
   executor thread is held while the job waits. The core knows nothing about
   processes or transports.
2. **Remote sub-graphs (optional target).** A coordinator sends a sub-graph of
   registered job kinds to a worker process and receives one reply. On the
   coordinator the whole request is one deferred job.

## Part 1: Deferred completion (core)

### API

```cpp
// include/sub0pipeline/completion.hpp
class Completion
{
public:
    Completion() noexcept = default;            // empty handle
    Completion(Completion&&) noexcept;
    Completion& operator=(Completion&&) noexcept; // abandons a held job first
    ~Completion();                              // abandons a held job

    void succeed() noexcept;
    void fail(PipelineError error, std::string_view message = {}) noexcept;
    void complete(std::expected<void, PipelineError> result,
                  std::string_view message = {}) noexcept;

    [[nodiscard]] std::stop_token stopToken() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
};

// Pipeline
[[nodiscard]] Job emplaceDeferred(std::function<void(Completion)> start);

// JobStatus (appended; existing values unchanged)
kAwaiting,  // The body returned; the job waits for its Completion.

// IObserver (new virtual with an empty default)
virtual void onJobAwaiting(RunId runId, JobId jobId, std::string_view jobName) {}
```

Usage (`req-deferred-completion`, `req-deferred-general`):

```cpp
auto read = pipe.emplaceDeferred([&io](Completion done) {
    io.readAsync(path, [done = std::move(done)](std::error_code ec) mutable {
        if (ec) done.fail(PipelineError::kJobFailed, ec.message());
        else    done.succeed();
    });
}).name("read").timeout(5s);
read.precede(decode);
```

The handle is move-only, so exactly one party can complete a job: the type
system rules out double completion. Destroying a handle that still holds a job
completes it with `kJobFailed` and the message "completion abandoned", in the
manner of a broken `std::promise`. A body that cannot start the work completes
the handle before it returns; that is the synchronous path, and costs no wait.

`stopToken()` returns the job's stop token. Pending work reaches cancellation
through a standard `std::stop_callback` on it (`req-deferred-cancellation`);
the library adds no second callback type.

### Lifecycle of a deferred job

A deferred job has three parties that can act on it during one run:

- **body**: the start function, called on an executor thread (or the run thread
  for inline executors); it returns once the work is started.
- **signal**: the holder of the `Completion` calls `complete()`, `succeed()`,
  `fail()`, or destroys the handle.
- **stop**: the job's stop source fires -- `Job::cancel()`, the run's external
  stop token, or the job's timeout.

Two decisions are made with atomics, each exactly once per run:

1. **Outcome.** The first of *signal* and *stop* wins a compare-exchange on the
   job's outcome word and records the result (`kSignalled` with the handle's
   result, or `kStopped` with `kCancelled` / `kTimeout`). The loser does
   nothing to the job.
2. **Finisher.** A party counter starts at 2. The *body* returning and the
   *outcome* winner each decrement it. Whoever reaches zero finishes the job:
   status, observer, failure record, successor release.

```
                 body running (kRunning)
                /                       \
   outcome before body returns     body returns first
   (synchronous completion,        -> kAwaiting, onJobAwaiting
    or stop during start)                  |
               |                   signal or stop wins outcome
     body returns: finish                  |
               \                     finish (via executor)
                \                         /
                 final status: kDone / kFailed / kCancelled / kTimedOut
```

States as seen through `status()`, `snapshot()` and the observer
(`req-deferred-observer`):

| Status | Meaning |
|---|---|
| `kReady` | Dependencies met, queued for dispatch |
| `kRunning` | The start function is executing |
| `kAwaiting` | The start function returned; the job waits for its outcome. Holds no thread |
| `kDone` / `kFailed` | Signalled with success / an error |
| `kCancelled` / `kTimedOut` | The stop party won the outcome |

`onJobAwaiting` is called by the body party before it decrements the party
counter and only when no outcome exists yet, so for each job it always precedes
`onJobFinish`. A job completed synchronously reports start and finish only.
Observers take their own timestamps, as for every other callback.

### Where finishing runs

| Finisher | Executor kind | Finish runs |
|---|---|---|
| body | any | in place, on the thread that ran the body |
| signal or stop | threaded (`runsInline() == false`) | as a continuation dispatched to the run's executor |
| signal or stop | inline (`runsInline() == true`) | on the run thread, which waits for it |

A continuation keeps observer callbacks, successor dispatch and deadline
cleanup off threads the library does not own: a transport's receive thread or a
timer thread only performs two atomic operations and one `dispatch()`. If the
executor rejects the continuation, the finish runs on the signalling thread
instead; that fallback defers stop-callback and deadline cleanup to the end of
the run, because the signalling thread may be inside the stop callback or the
deadline expiry that cleanup would wait for.

An inline run keeps its single-thread property: every job body, every finish
and every observer callback runs on the run thread. When the ready list is
empty and deferred jobs remain, the run thread waits on a condition variable
for the next finished job. A run on `SequentialExecutor` or `runInline()` is
therefore still deterministic in the order outcomes arrive.

### run() waits for deferred jobs (`req-no-thread-wait`)

`IExecutor::waitAll()` returns once no body is executing, which is too early
when deferred jobs are pending. For a graph with deferred jobs, `run()` loops:

```
executor.waitAll();
loop:
    wait until deferredInFlight == 0      (run thread only, no executor thread)
    executor.waitAll();                   (continuations and their successors)
    if deferredInFlight == 0: done
```

`deferredInFlight` increments inside the body's executor callback and
decrements as the last action of a finish, after successors were dispatched.
So when it reads zero, every dispatch it caused is already counted by the
executor, and the following `waitAll()` covers it. A graph without deferred
jobs skips the loop.

No executor thread waits on a pending job: the run thread waits, as it already
does in `waitAll()`. The test with more pending deferred jobs than pool threads
pins this.

### Cancellation, timeout, late completion and orphans (`req-deferred-cancellation`)

When stop wins the outcome, the job finishes as `kCancelled` (or `kTimedOut`)
without waiting for the work: successors are skipped and `run()` can return.
The pending work may still use borrowed state, so it becomes an **orphan**, the
same term and contract as a timed-out plain job:

- `hasPendingOrphans()` is true until the handle is completed or destroyed.
- `joinOrphans()` blocks until every orphaned handle is released.
- The next `run()` and the Pipeline's destructor join orphans first, so a late
  completion can never reach a later run.
- A late completion is ignored: it loses the outcome compare-exchange, and its
  only effect is to release the orphan.

Orphan accounting uses one flag word per job. The handle sets `released` when
it is completed or destroyed. The finisher of a stopped job sets `orphaned`
with a compare-exchange that fails if `released` is already set, and increments
the orphan count under the orphan mutex before that compare-exchange becomes
visible. A releasing handle that sees `orphaned` decrements the count under the
same mutex and notifies `joinOrphans()`.

Timeouts measure from the start of the body to the outcome. With an injected
`IDeadlineService` the job arms a `Deadline` on its own stop source. Without
one, the Pipeline lazily starts one internal timer thread that serves every
timed deferred job (a sorted queue, not a thread per job). `cancelAndWait()`
runs only on executor threads or the run thread, never inside a stop callback
or an expiry, so it cannot wait for itself. A timed-out job reports `kTimeout`
even when the work signals its own `kCancelled` first in response to the stop,
matching cooperative jobs.

A fatal failure elsewhere in the graph does not cancel a pending deferred job,
the same as a running cooperative job: the run waits for it. Use the external
stop token or `Job::cancel()` to abandon it.

### Threading and memory-ordering rules

- The outcome compare-exchange is `acq_rel`. The winner writes the result and
  message, then decrements the party counter with `acq_rel`; the finisher reads
  them after its own decrement. No other party touches result or message.
- After its decrement, a non-finishing party does not touch the job again. The
  handle sets `released` before it decrements, so a later run can never see a
  stale write.
- The finish decrements `deferredInFlight` under the run's mutex and notifies
  while holding it; after unlocking it touches no run state.
- Status transitions use the existing atomic `jobStatus_`. The body moves
  `kRunning -> kAwaiting` with a compare-exchange, so it never overwrites a
  final status stored by a finisher on another thread.
- Re-runs: the run initialization renews every deferred job's stop source, and
  the thread that runs the body resets the per-run state before it creates the
  `Completion`. This is safe because the previous run's orphans were joined
  first and every non-orphan handle was released.
- At the end of a run, the run thread releases every deferred job's stop
  callback and deadline registration that a finish could not release itself
  (the executor-rejection fallback).

### Costs

- Graph construction: one heap slot per deferred job (start function, per-run
  state, stop callback and deadline storage). Plain jobs pay one pointer in the
  node (still within the existing size budget).
- Per run of a deferred job: a stop callback registration, one mutex
  lock/unlock pair at start and at finish, and for an asynchronous outcome one
  executor dispatch. A failure message allocates, as `setCurrentJobError` does.
- Graphs without deferred jobs: one branch on the deferred flag in dispatch and
  an empty-list check at the end of `run()`.

### Measurements

Release build, MSVC 19.51, i7-11800H, Windows 11, `Sub0Pipeline_Bench`, process
pinned and at high priority, medians of five to nine alternating samples. These
are no-op job figures from one host, not application throughput.

| Case | Result |
|---|---|
| Plain jobs, library change only (same benchmark source), 10-job chain / 1000-job layered DAG | about +3.5% per job versus the previous core |
| 10 deferred jobs completed before the body returns, inline executor | 2.3 us per run, about 170 ns more per job than a plain job |
| 10-job deferred chain completed on a separate thread, `PriorityExecutor` with 4 threads | 230 us per run (median), about 23 us per hand-off including two thread wake-ups |

Routing plain jobs through the shared deferred finish measured 5-15% slower per
job, because MSVC does not inline a function that holds a try block; plain jobs
therefore keep their own inlined finish.

### Scope limits

- On-demand jobs (`addOnDemand`) cannot be deferred in this version: `trigger()`
  has no run thread to wait for an outcome.
- The DSL has no deferred spec yet; use `Pipeline::emplaceDeferred`.
- The core uses standard threads for the fallback timer, as it already does for
  native timeouts. Builds without exceptions are supported: no new path throws.

## Part 2: Remote sub-graphs (optional target, proposed)

### Target layout (`req-optional-build`)

A separate CMake target `Sub0Pipeline::Remote`, enabled with
`SUB0PIPELINE_BUILD_REMOTE`, headers under `include/sub0pipeline/remote/`. It
depends on the core only through public API (`emplaceDeferred`, `Completion`,
`Pipeline`, `ScopedExecutor`). A consumer that does not link it sees no change.

| Header | Contents |
|---|---|
| `remote/transport.hpp` | `ITransport`, `IFrameSink`, `TransportError` |
| `remote/loopback_transport.hpp` | `LoopbackTransport::pair()` for tests and in-process use |
| `remote/registry.hpp` | `JobKindId`, `JobRegistry`, `registryFingerprint()` |
| `remote/payload.hpp` | `Payload` (inline bytes or opaque handle), `IPayloadStore` |
| `remote/subgraph.hpp` | `SubGraphRequest`, `SubGraphReply` |
| `remote/channel.hpp` | `RemoteChannel` (coordinator side) |
| `remote/worker_host.hpp` | `WorkerHost` (worker side) |

### Sub-graph request (`req-remote-subgraph`, `req-remote-node`)

```cpp
SubGraphRequest request;
auto a = request.add({"decode", 1}, Payload::handle(sourceKey));
auto b = request.add({"resize", 2}, Payload::inlineBytes(settings));
request.edge(a, b);               // b receives a's output
request.exportOutput(b);          // only exported outputs travel back

Job remote = channel.emplace(pipe, std::move(request),
    [&](const SubGraphReply& reply) { results.take(reply); });
remote.name("remote-resize").succeed(prepare);
```

`RemoteChannel::emplace` adds one deferred job whose body sends the request and
moves its `Completion` into the channel's in-flight table. A single remote job
is a one-node request; there is one format and one worker loop. The caller
decides the batch size by how many nodes it puts in one request.

Inside a request, an edge both orders jobs and passes data: each job receives
its own input payload plus the outputs of its predecessors, in edge order.
Only outputs marked exported are sent back, so intermediates never cross the
boundary.

### Registry (`req-job-registry`)

```cpp
struct JobKindId { std::string name; uint32_t version; };

using JobKindFn = std::function<std::expected<void, PipelineError>(JobContext&)>;
JobRegistry registry;
registry.add({"decode", 1}, decodeFn);
```

`JobContext` gives the job its input `Payload`, its predecessors' outputs, an
output sink, the store and its stop token. No code travels: a request names
kinds, and a worker runs only kinds it registered. An unknown name or version
fails that job with `kJobFailed` and the message "unknown job kind decode@1";
the rest of the request is skipped as a required failure would be.

`registryFingerprint()` is FNV-1a 64 over the kinds sorted by name then version,
each encoded as in the wire format. The coordinator computes it from the kind
list it was built against, without factories.

### Payloads and the store (`req-payload-handles`)

```cpp
class Payload { /* kind: inline bytes or handle bytes; at most one owner */ };

class IPayloadStore
{
public:
    virtual ~IPayloadStore() = default;
    // Resolve a handle to readable bytes; the view stays valid until release().
    [[nodiscard]] virtual std::expected<PayloadView, PipelineError>
        resolve(std::span<const std::byte> handle) = 0;
    virtual void release(PayloadView view) noexcept = 0;
    // Store bytes and return a handle for them.
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, PipelineError>
        put(std::span<const std::byte> bytes) = 0;
};
```

Handles are opaque byte strings of at most 64 bytes (a content hash fits). The
library never inspects them. Inline payloads are limited to a size agreed in
the handshake (default 64 KiB); a larger output is put into the store and sent
as a handle. A large payload therefore never travels through the transport.

### Wire format (`req-transport-interface`, `req-handshake`)

All integers little-endian, fixed width. Strings are `u16 length + bytes`
(UTF-8, not terminated). Byte blobs are `u32 length + bytes`.

```
frame   := u32 bodyLength | u8 type | body
type    := 1 Hello | 2 HelloAck | 3 HelloReject | 4 Request | 5 Reply
         | 6 Cancel | 7 Goodbye

Hello       := u32 magic "S0PL" | u16 protocolMajor | u16 protocolMinor
             | u64 registryFingerprint | u32 maxInFlight | u32 maxInlineBytes
HelloAck    := same fields, the worker's values
HelloReject := u8 reason (1 version, 2 fingerprint) | string detail
Request     := u64 requestId | u32 jobCount | job* | u32 edgeCount | edge*
job         := string kindName | u32 kindVersion | u8 flags(bit0 exported)
             | payload
edge        := u32 from | u32 to            (indices into job*)
payload     := u8 kind (0 none, 1 inline, 2 handle) | blob
Reply       := u64 requestId | u32 jobCount | result*
result      := u8 JobStatus | u8 PipelineError | string message | payload
Cancel      := u64 requestId
Goodbye     := u8 reason
```

Versioning: a different `protocolMajor` is incompatible; a higher
`protocolMinor` may append fields to a frame body, which an older reader skips
by `bodyLength`. Unknown frame types close the connection.

Handshake: the coordinator sends `Hello`; the worker answers `HelloAck` or
`HelloReject`. A major-version or fingerprint mismatch fails the connection
before any request is sent; `RemoteChannel::connect()` returns the reason. Both
sides then use `min(maxInFlight)` and `min(maxInlineBytes)`.

The library owns framing and encoding. `ITransport` moves whole frames:

```cpp
class ITransport
{
public:
    virtual ~ITransport() = default;
    virtual void start(IFrameSink& sink) = 0;   // sink.onFrame / sink.onClosed
    [[nodiscard]] virtual std::expected<void, TransportError>
        send(std::span<const std::byte> frame) = 0;   // thread-safe
    virtual void close() noexcept = 0;
};
```

A byte-stream transport (pipe, socket) reassembles frames with the shipped
`FrameReader` helper using `bodyLength`. The loopback transport is a pair of
in-memory queues, each with one delivery thread. A desktop pipe or socket
transport may ship later as its own target.

### Error model (`req-error-propagation`, `req-worker-failure`)

| Failure | Coordinator job result | Detail |
|---|---|---|
| One or more jobs in the sub-graph fail | `kJobFailed` (or the job's own error) | Message names the first failed job and its text |
| Unknown kind or version | `kJobFailed` | "unknown job kind name@version" |
| Worker cancelled the request after a `Cancel` | ignored (coordinator already cancelled) | late reply dropped |
| Transport closed or worker stopped | `kUnavailable` (proposed new core enumerator) | every in-flight and queued request fails |
| Handshake mismatch | `connect()` fails; no job is created | reason and detail |

"First failure in a defined order" means the lowest job index among failed
jobs in the request, not the order failures happened in, so the result does not
depend on worker scheduling (`req-determinism`). `firstFailureName()` on the
coordinator names the remote job; the reply handler receives every job's result.

### Back-pressure (`req-backpressure`)

Each channel has an in-flight limit (the handshake minimum) and a bounded wait
queue with a configured capacity. A remote job that starts while the channel is
full waits in the queue holding only its `Completion`, no thread. When the queue
is full too, the job fails immediately with `kBusy`. `freeCapacity()` returns
free in-flight slots plus free queue slots, so a graph builder can size batches.

### Cancellation across the boundary

The deferred job's stop callback sends `Cancel{requestId}`. The worker requests
stop on that request's run. The coordinator job finishes as cancelled at once
(core behaviour); when the worker's reply arrives the channel drops it and
releases the `Completion`, which releases the orphan.

### Worker loop (`req-worker-runtime`)

```cpp
WorkerHost host{registry, store, transport,
                {.executor = &pool, .maxConcurrentRequests = 4, .queueCapacity = 16}};
host.serve(stopToken);   // returns after Goodbye, close or stop
```

The receive thread decodes a request and pushes it to a bounded queue. Each of
`maxConcurrentRequests` runner threads pops one, builds a fresh `Pipeline` from
the registry, and runs it on a `ScopedExecutor` over the shared pool, so jobs of
one request run in parallel and one request's `waitAll()` does not wait for
another's jobs. The runner encodes the reply and sends it. The worker never
starts processes; the consumer starts workers and hands each side its transport.

### Tests (`req-tests`) and benchmarks (`req-overhead`)

Loopback end-to-end runs of 1-job and multi-job sub-graphs with data passed
along edges; registry mismatch and fingerprint mismatch at handshake; unknown
kind; per-job error propagation and its defined order; worker loss with
requests in flight and queued; back-pressure at limit and at queue capacity;
cancel round trip with late reply; more pending remote jobs than coordinator
pool threads while CPU jobs finish; equal outputs remote and in process.
Benchmarks: round trip per request and per job over loopback for sub-graphs of
1, 16 and 256 jobs; targets are set after the first measurement.

## Alternatives considered

- **Blocking proxy job.** A plain job that sends a request and waits for the
  reply. Rejected: it holds an executor thread per pending request, which is
  the problem to solve (`req-no-thread-wait`).
- **Executor-level remoting.** An `IExecutor` whose `dispatch()` runs jobs in
  another process. Impossible: `dispatch()` receives `std::function<void()>`
  closures, which cannot be serialised, and no code may travel over the wire
  (`req-job-registry`).
- **Per-job requests without sub-graphs.** One request per remote job. Rejected
  as the only form: a chain of N jobs costs N round trips and sends each
  intermediate payload twice. A one-node sub-graph covers the single-job case
  with the same format.
- **Completion by job id** (`Pipeline::complete(JobId, result)` plus a special
  "deferred" return value). Rejected: ids repeat across runs, so a late call
  could complete the next run's job, and nothing prevents double completion.
  A move-only handle makes both impossible.
- **Finishing on the signalling thread** for threaded executors. Rejected as the
  default: observer callbacks, successor dispatch and deadline cleanup would run
  on transport or timer threads, and `cancelAndWait()` could be reached from
  inside the expiry it waits for. Kept only as the rejection fallback.
- **Waiting for the work to acknowledge cancellation.** Rejected: a stuck worker
  would stall the run; finishing at once and tracking the work as an orphan
  reuses the existing timed-out-job contract.
- **Coroutines (`co_await`).** Not chosen for the core: they add a frame
  allocation per job and a language-level dependency on all targets. A
  `Completion` can be wrapped by an awaiter in user code.

## Requirement trace

| Handle | Where satisfied |
|---|---|
| `req-deferred-completion` | Part 1 API; lifecycle; where finishing runs |
| `req-deferred-cancellation` | Cancellation, timeout, late completion and orphans |
| `req-deferred-observer` | Lifecycle states table; `onJobAwaiting`; `kAwaiting` in `snapshot()` |
| `req-deferred-general` | Part 1 has no process or transport types; I/O, GPU and timer usage |
| `req-no-thread-wait` | run() waits for deferred jobs; test with more pending jobs than threads |
| `req-optional-build` | Part 1 scope limits (no-exceptions build); Part 2 target layout |
| `req-remote-subgraph` | Sub-graph request |
| `req-remote-node` | Sub-graph request (`RemoteChannel::emplace`) |
| `req-job-registry` | Registry |
| `req-worker-runtime` | Worker loop |
| `req-payload-handles` | Payloads and the store |
| `req-error-propagation` | Error model |
| `req-worker-failure` | Error model |
| `req-backpressure` | Back-pressure |
| `req-handshake` | Wire format, handshake |
| `req-transport-interface` | Wire format, `ITransport`, loopback |
| `req-determinism` | Error model (index order); no ids in payloads |
| `req-overhead` | Tests and benchmarks |
| `req-tests` | Part 1 tests in `tests/test_deferred.cpp`; Part 2 test plan |
| `req-docs` | README sections for both features |

## Open questions

- Add `PipelineError::kUnavailable` to the core for worker loss, or report it
  through the reply handler only? A distinct core code lets generic retry logic
  work without the remote target's types.
- Should exported outputs be returned as handles always (store-only), removing
  the inline path on the reply side?
- Does the worker need per-kind concurrency limits in addition to the
  per-request limit?
