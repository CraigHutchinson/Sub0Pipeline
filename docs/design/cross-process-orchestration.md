# Cross-process orchestration: deferred completion and remote sub-graphs

Status: both parts are implemented. Deferred completion is in the core
library; remote sub-graphs are the optional `Sub0Pipeline::Remote` target.

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
| Plain jobs, library change only (same benchmark source), 10-job chain / 1000-job layered DAG | about +3.5% per job versus the previous core with a duplicated finish; see the performance note for the single finish path now used |
| 10 deferred jobs completed before the body returns, inline executor | 2.3 us per run, about 170 ns more per job than a plain job |
| 10-job deferred chain completed on a separate thread, `PriorityExecutor` with 4 threads | 230 us per run (median), about 23 us per hand-off including two thread wake-ups |

#### Performance note: one finish path

Every job, plain or deferred, finishes through one function, `finishJob()`. It
holds no try block -- the executor-rejection boundary lives in `submitReady()` --
so it can carry `SUB0PIPELINE_FORCEINLINE`. Measured against a hand-inlined copy
of the finish in `dispatchJob()` (pinned, high priority, medians of seven):

| Case | Copy | Shared, force-inlined | Shared, no force-inline |
|---|---|---|---|
| 10-job chain, nested dispatch | 448 ns | 503 ns (+12%) | 590 ns (+32%) |
| 1000-job layered DAG, nested dispatch | 50.9 us | 55.5 us (+9%) | 62.2 us (+22%) |
| `runInline`, 1000-job layered DAG | 43.2 us | 42.1 us (within noise) | -- |
| `PriorityExecutor` (4), 10-job chain | 17.7 us | 16.7 us (within noise) | 18.2 us |

Against the core before deferred jobs (same method): 10-job chain with nested
dispatch 431 -> 508 ns (+18%), 1000-job layered DAG with nested dispatch 52.1 ->
56.6 us (+9%), `runInline` 1000-job DAG +1.5%, `PriorityExecutor` cases +0.4% and
+3.6%.

Also tried and not better than the force-inlined shared function: keeping the
try block in a wrapper around the shared function, and moving the failure
branch into a separate non-inlined function. The cost shows only where an
executor nests one dispatch inside another; threaded executors and inline runs
are within noise. Duplicating the finish into `dispatchJob()` is therefore a
known lever, to pull only if the library's overall performance targets later
demand it; one implementation is kept for maintainability.

### Scope limits

- On-demand jobs (`addOnDemand`) cannot be deferred in this version: `trigger()`
  has no run thread to wait for an outcome.
- The DSL has no deferred spec yet; use `Pipeline::emplaceDeferred`.
- The core uses standard threads for the fallback timer, as it already does for
  native timeouts. Builds without exceptions are supported: no new path throws.

## Part 2: Remote sub-graphs (optional target)

### Target layout (`req-optional-build`)

A separate CMake target `Sub0Pipeline::Remote`, built when
`SUB0PIPELINE_BUILD_REMOTE` is on (the default), with headers under
`include/sub0pipeline/remote/`. It uses the core only through public API
(`emplaceDeferred`, `Completion`, `Pipeline`, `ScopedExecutor`). A consumer that
does not link it sees no change; the only core addition it needed is the
`PipelineError::kUnavailable` enumerator. Its sources compile with
`-fno-exceptions`.

| Header | Contents |
|---|---|
| `remote/payload.hpp` | `Payload`, `PayloadKind`, `kMaxHandleBytes`, `IPayloadStore` |
| `remote/registry.hpp` | `JobKindId`, `JobContext`, `JobKindFn`, `JobRegistry`, `registryFingerprint()` |
| `remote/subgraph.hpp` | `SubGraphRequest`, `JobResult`, `SubGraphReply` |
| `remote/transport.hpp` | `ITransport`, `IFrameSink`, `TransportError`, `FrameReader` |
| `remote/loopback_transport.hpp` | `LoopbackTransport::pair()` |
| `remote/wire.hpp` | Frame types, `Hello`, `HelloReject`, `wire::` encoders and decoders |
| `remote/channel.hpp` | `RemoteChannel`, `ConnectError`, `ReplyHandler` (coordinator side) |
| `remote/worker_host.hpp` | `WorkerHost` (worker side) |

### Sub-graph request (`req-remote-subgraph`, `req-remote-node`)

```cpp
SubGraphRequest request;
auto a = request.add({"decode", 1}, Payload::handle(sourceKey));
auto b = request.add({"resize", 2}, Payload::inlineBytes(settings), /*exported=*/true);
request.edge(a, b);               // b runs after a and receives a's output

Job remote = channel.emplace(pipe, request,
    [&](const SubGraphReply& reply) { results.take(reply.results[b].output); });
remote.name("remote-resize").succeed(prepare);
```

`RemoteChannel::emplace` adds one deferred job whose body sends the request and
moves its `Completion` into the channel's in-flight table. A single remote job
is a one-node request; there is one format and one worker loop. The caller
decides the batch size by how many jobs it puts into one request. The reply
handler runs on the transport's thread before the job completes, so the
dependants of the remote job see what it stored.

Inside a request, an edge both orders jobs and passes data: each job receives
its own input and the outputs of its predecessors, in edge order
(`JobContext::predecessor(i)`). Only outputs marked exported are sent back.

### Registry (`req-job-registry`)

```cpp
JobRegistry registry;
registry.add({"decode", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
    auto bytes = ctx.store()->get(ctx.input().bytes());   // resolve a handle input
    ...
    ctx.setOutput(Payload::inlineBytes(std::move(decoded)));
    return {};
});
```

`JobContext` gives the job its input, its predecessors' outputs, an output
slot, `setError()` for diagnostic text, the store and a stop token. No code
travels: a request names kinds, and a worker runs only kinds it registered. An
unknown name or version fails that job with `kJobFailed` and the message
"unknown job kind name@version".

`registryFingerprint()` is FNV-1a 64 over the kinds sorted by name, then
version, each encoded as on the wire (u16 length, name, u32 version). The
coordinator computes it from the kind list it was built against.

### Payloads and the store (`req-payload-handles`)

A `Payload` is nothing, inline bytes, or an opaque handle of at most 64 bytes
(a content hash fits). `IPayloadStore` has `get(handle) -> bytes` and
`put(bytes) -> handle`, supplied by the consumer on both sides over a shared
store. The library never interprets handles and never resolves an input
handle itself: a job calls `ctx.store()->get()` only when it needs the bytes.

Inline payloads are limited to the smaller of both sides' `maxInlineBytes`
(default 64 KiB). The coordinator refuses a request with a larger inline input
(`kJobFailed`). The worker turns a larger exported output into a handle through
its store. Successors inside the request still received the inline bytes in
memory. Without a store, that job fails with a message saying so. A large
payload therefore never travels through the transport.

### Wire format (`req-transport-interface`, `req-handshake`)

All integers little-endian, fixed width. Strings are `u16 length + bytes`;
blobs are `u32 length + bytes`.

```
frame   := u32 bodyLength | u8 type | body
type    := 1 Hello | 2 HelloAck | 3 HelloReject | 4 Request | 5 Reply | 6 Cancel

Hello       := u32 magic "S0PL" | u16 protocolMajor | u16 protocolMinor
             | u64 registryFingerprint | u32 maxInFlight | u32 maxInlineBytes
HelloAck    := same fields, the worker's values
HelloReject := u8 reason (1 version, 2 fingerprint) | string detail
Request     := u64 requestId | u32 jobCount | job* | u32 edgeCount | edge*
job         := string kindName | u32 kindVersion | u8 flags(bit0 exported) | payload
edge        := u32 from | u32 to            (indices into job*, checked on decode)
payload     := u8 kind (0 none, 1 inline, 2 handle) | blob
Reply       := u64 requestId | u32 jobCount | result*
result      := u8 JobStatus | u8 PipelineError | string message | payload
Cancel      := u64 requestId
```

Versioning: a different `protocolMajor` is incompatible. A higher
`protocolMinor` may append fields to a hello, which an older reader skips.
A malformed frame or an unknown type closes the connection.

Handshake: `RemoteChannel::connect()` sends `Hello` with the fingerprint it
expects; the worker answers `HelloAck` or `HelloReject` and then closes. A
major-version or fingerprint mismatch fails `connect()` with
`kVersionMismatch` / `kFingerprintMismatch` before any request is sent. Both
sides then use the smaller `maxInFlight` and `maxInlineBytes`.

The library owns framing and encoding; `ITransport` moves whole frames
(`start(sink)`, `send(frame)`, `close()`, with `onFrame` / `onClosed(reason)`
delivered one at a time, `onClosed` exactly once and last). `FrameReader`
reassembles frames from a byte stream for pipe or socket transports.
`LoopbackTransport` is an in-memory pair with one delivery thread per end;
closing either end delivers frames already sent and then closes both. A desktop
pipe or socket transport is not shipped yet.

### Error model (`req-error-propagation`, `req-worker-failure`, `req-determinism`)

| Failure | Coordinator job result | Detail |
|---|---|---|
| Jobs in the sub-graph fail | The lowest failed index's error (`kCancelled` / `kTimeout` for those statuses) | Message: "job 1 (fail@1) failed: text"; the reply handler sees every result |
| Unknown kind or version | `kJobFailed` | "unknown job kind name@version" |
| Inline input over the limit, or a request the format cannot carry | `kJobFailed` | Refused before sending |
| Coordinator cancels | `kCancelled` (core rules) | The worker stops the request; its late reply is dropped |
| Transport closed or worker stopped | `kUnavailable` | Every in-flight and queued request; later submissions too |
| Channel full | `kBusy` | See back-pressure |
| Handshake mismatch | `connect()` fails; jobs on the channel fail `kUnavailable` | |

On the worker every job runs as optional and skips itself when a predecessor
did not succeed. A required failure would stop independent branches at
whatever point scheduling had reached; with optional jobs, which jobs run and
which fail depends only on the inputs. The remote job then takes the failure
with the lowest job index (`SubGraphReply::firstFailure()`), so its result does
not depend on worker scheduling. `firstFailureName()` on the coordinator names
the remote job.

### Back-pressure (`req-backpressure`)

Each channel allows the negotiated in-flight limit (the smaller of
`Options::maxInFlight` and the worker's `maxConcurrentRequests`). Beyond it, a
request waits in a queue of `Options::queueCapacity`, holding only its
`Completion` and no thread; a queued request whose job is cancelled is never
sent. When the queue is full -- immediately, with the default capacity 0 -- the
job fails with `kBusy`. `freeCapacity()` returns free in-flight slots plus free
queue slots. A worker answers a request beyond its own limit with every job
failed `kBusy`, which a coordinator that honours the handshake never sees.

### Cancellation across the boundary

The channel registers a stop callback on the job's stop token after the
request is sent, so a cancel never overtakes its request. The callback sends
`Cancel{id}`. The worker requests stop on that request's run, whether it is
running or still queued, and replies; the reply frees the coordinator's slot
and releases the orphan. The coordinator's job already finished as cancelled
(core rules); the reply handler is not called for it.

### Worker loop (`req-worker-runtime`)

```cpp
WorkerHost host{registry, transport,
                {.executor = &pool, .maxConcurrentRequests = 4, .store = &store}};
host.serve(stopToken);   // returns after the transport closes or stop is requested
```

The transport's thread decodes each request into a bounded queue (bounded by
`maxConcurrentRequests`). `maxConcurrentRequests` runner threads each take one,
build a fresh `Pipeline` from the registry, and run it on a `ScopedExecutor`
over the shared pool. The jobs of one request run in parallel, and one
request's run never waits for another's jobs. The runner encodes and sends the
reply. `serve()` stops running requests and joins its runners before it
returns. The library never starts processes; the consumer starts workers and
gives each side its transport.

### Current choices (revisit if needs change)

| Topic | Current choice | Reason | What would change it |
|---|---|---|---|
| Worker-loss error | New core `PipelineError::kUnavailable` | Generic retry logic recognises it without the remote target's types, and it reads correctly for other resources that go away | A consumer needing the transport's close reason in the job result; then add it to the reply handler or a channel query, keeping the code |
| Reply payloads | Inline up to the smaller limit both ways; larger exported outputs as store handles | Small outputs avoid a store round trip; large ones never cross the transport | A store-only consumer (always handles), or a measured cost of inline copies for mid-sized outputs |
| Worker limits | One per-request concurrency limit, no per-kind limits | Kinds share one executor pool, which already bounds CPU; nothing yet needs per-kind isolation | A kind with an exclusive resource (a device, a licence, a large memory footprint) that must run alone; add a per-kind semaphore in the worker |
| Full channel | Fail fast with `kBusy`; opt-in bounded queue (`queueCapacity`) | The caller decides retry and back-off; the queue keeps large graphs simple without unbounded growth | Graph builders consistently wanting automatic waiting; make a non-zero queue the default |

### Tests and benchmarks (`req-tests`, `req-overhead`)

`tests/test_remote.cpp` (16 cases) runs over the loopback transport: registry
duplicates, lookup and order-free fingerprints; wire round-trips and refusal of
truncated frames and oversized handles; frame reassembly from single bytes; one-job
and multi-job requests with outputs along edges and only exported outputs
returned; outputs equal to running the same kinds in process; fingerprint and
protocol-major mismatches; unknown kinds; per-job errors and the lowest-index
rule; worker loss with requests in flight; fail-fast `kBusy` and the bounded
queue; cancellation reaching the worker; more awaiting remote jobs than
coordinator threads while CPU jobs finish; large outputs as store handles;
re-runs.

Round trip over loopback, no-op jobs, worker pool of four threads, MSVC 19.51
Release, i7-11800H, pinned at high priority, medians of five (`req-overhead`).
These are the first measurements; later changes should hold them.

| Sub-graph | Per request | Per job |
|---|---|---|
| 1 job | 51.8 us | 51.8 us |
| 16 jobs | 72.5 us | 4.5 us |
| 256 jobs | 555 us | 2.2 us |

The fixed cost is dominated by two thread hand-offs per direction (loopback
delivery thread, runner thread, executor, continuation); batching jobs into one
request amortises it.

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
| `req-determinism` | Error model (optional worker jobs, index order); no ids in payloads; in-process equality test |
| `req-overhead` | Tests and benchmarks (1, 16, 256 jobs over loopback) |
| `req-tests` | `tests/test_deferred.cpp`; `tests/test_remote.cpp` |
| `req-docs` | README sections for both features |

## Open questions

Each topic below has a current choice, recorded with its reason in "Current
choices (revisit if needs change)": the worker-loss error code, reply-side
handles versus inline payloads, per-kind worker limits, and fail-fast `kBusy`
versus waiting. Still open beyond those:

- A shipped desktop transport (named pipe / Unix socket) as its own target.
- A DSL form for deferred and remote jobs.
