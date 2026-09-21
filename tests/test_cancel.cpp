// tests/test_cancel.cpp
//
// Cancellation and timeout tests.

#include <sub0pipeline/sub0pipeline.hpp>
#include "test_helpers.hpp"
#include "doctest.h"

#include <chrono>

namespace sub0pipeline { std::unique_ptr<IExecutor> makeDesktopExecutor(); }

using namespace sub0pipeline;
using namespace std::chrono_literals;

// ═══════════════════════════════════════════════════════════════════════════════
// Cancellation
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Cancel: cancellable job receives stop_token")
{
    RecordingExecutor exec;
    Pipeline pipe;

    bool tokenReceived = false;
    auto j = pipe.emplace([&tokenReceived](std::stop_token st) -> std::expected<void, PipelineError> {
        tokenReceived = true;
        return {};
    });
    j.name("cancellable");

    auto result = pipe.run(exec);
    CHECK(result.has_value());
    CHECK(tokenReceived);
}

TEST_CASE("Cancel: kCancelled error code propagates correctly when job self-reports")
{
    // cancel() is designed for mid-run concurrent use; resetNode() creates a
    // fresh stop_source per epoch so pre-run cancel() is a no-op by design.
    // This test verifies the kCancelled error path via job self-report.
    RecordingExecutor exec;
    Pipeline pipe;

    auto j = pipe.emplace([](std::stop_token) -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kCancelled);
    });
    j.name("fetch");

    auto result = pipe.run(exec);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCancelled);
    CHECK(pipe.status(j) == JobStatus::kCancelled);
}

TEST_CASE("Cancel: pre-run cancel() is a no-op -- stop_source resets at run() start")
{
    // resetNode() creates a fresh stop_source per epoch so that cancellation
    // state from a previous run (or a mistaken pre-run cancel) does not bleed
    // into the next run. cancel() is intended for concurrent mid-run use.
    RecordingExecutor exec;
    Pipeline pipe;

    int ran = 0;
    auto j = pipe.emplace([&ran](std::stop_token st) -> std::expected<void, PipelineError> {
        if (st.stop_requested()) return std::unexpected(PipelineError::kCancelled);
        ++ran;
        return {};
    });
    j.cancel();  // fired before run() -- will be reset by resetNode()

    auto result = pipe.run(exec);
    CHECK(result.has_value());  // cancel had no effect
    CHECK(ran == 1);
}

TEST_CASE("Cancel: non-cancellable job ignores cancel() entirely")
{
    RecordingExecutor exec;
    Pipeline pipe;

    int ran = 0;
    auto j = pipe.emplace([&ran]() -> std::expected<void, PipelineError> {
        ++ran; return {};
    });
    j.cancel();

    auto result = pipe.run(exec);
    CHECK(result.has_value());
    CHECK(ran == 1);
}

TEST_CASE("Cancel: stop_source is fresh on each run -- no cross-epoch bleed")
{
    RecordingExecutor exec;
    Pipeline pipe;

    int runCount = 0;
    auto j = pipe.emplace([&runCount](std::stop_token st) -> std::expected<void, PipelineError> {
        if (st.stop_requested()) return std::unexpected(PipelineError::kCancelled);
        ++runCount;
        return {};
    });

    // Run 1 clean
    auto r1 = pipe.run(exec);
    CHECK(r1.has_value());
    CHECK(runCount == 1);

    // Run 2 also clean -- no stop_source bleed from run 1
    auto r2 = pipe.run(exec);
    CHECK(r2.has_value());
    CHECK(runCount == 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Timeout
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Timeout: cancellable job honours stop_token fired by DesktopExecutor watchdog")
{
    auto exec = sub0pipeline::makeDesktopExecutor();
    Pipeline pipe;

    // Job polls stop_token in a tight loop; watchdog fires it after 50ms.
    auto j = pipe.emplace([](std::stop_token st) -> std::expected<void, PipelineError>
    {
        while (!st.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        return std::unexpected(PipelineError::kCancelled);
    });
    j.name("slow_fetch").timeout(50ms);

    auto result = pipe.run(*exec);
    CHECK_FALSE(result.has_value());
    // Either kCancelled (cooperative exit via stop_token) or kTimeout (hard cutoff)
    CHECK((result.error() == PipelineError::kCancelled
        || result.error() == PipelineError::kTimeout));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Structured cancellation -- groundwork for issue #1
// ═══════════════════════════════════════════════════════════════════════════════
//
// Pouncer's validate -> durable-commit -> device-ACK pipeline needs a hard
// answer to: "if cancellation arrives in the window between commit
// succeeding and its ACK stage running, does the ACK stage still run?"
// These tests pin that answer down deterministically (no wall-clock sleeps,
// no real threads -- run_inline()/SequentialExecutor only) and exercise the
// two structured-cancellation signals this groundwork adds:
//   1. "cancellation requested"    -- run()/run_inline() return kCancelled.
//   2. "safe to free borrowed state" -- join_orphans() / has_pending_orphans().

TEST_CASE("Cancel: KNOWN GAP -- Job::cancel() on a not-yet-reached successor is swallowed by its own epoch reset")
{
    // This documents a genuine, still-open gap discovered while building the
    // external-stop-token fix below (see the next test for the mechanism
    // that IS fixed). resetNode() -- see src/sub0pipeline.cpp -- lazily
    // assigns each node a *fresh* std::stop_source the first time that node
    // is touched in the current epoch, specifically so that a cancel() from
    // a PREVIOUS run (or before run() was called at all) cannot bleed into
    // a new one ("Cancel: pre-run cancel() is a no-op" above).
    //
    // But that same reset fires for a successor's *first* touch during the
    // CURRENT run too -- which happens lazily, only when its predecessor
    // completes (Pipeline::runImpl's successor-dispatch loop). If
    // Job::cancel() is called on that successor's handle *before* its
    // predecessor finishes (i.e. exactly the "cancelled between
    // commit-success and ACK-dispatch" window from issue #1, but delivered
    // via a raw Job handle rather than an external token), the request lands
    // on a stop_source that resetNode() is about to unconditionally replace
    // -- so it is silently discarded and ack still runs.
    //
    // The external-stop_token overload added by this groundwork (next test)
    // does NOT have this gap, because resetNode() consults the *live*
    // external std::stop_token at reset time rather than relying on a
    // per-node flag that reset can stomp on. Closing this gap for the
    // Job::cancel()-on-a-handle path too would need the node to remember
    // "a stop was requested during epoch N" independent of resetNode's
    // stop_source replacement (e.g. a per-node cancel-epoch marker) --
    // left as follow-up, not attempted here.
    Pipeline pipe;
    bool committed = false;
    bool ackRan    = false;

    Job ack; // assigned below; captured by reference so commit's body can cancel it
    auto commit = pipe.emplace([&]() -> std::expected<void, PipelineError>
    {
        committed = true;
        ack.cancel();  // "cancellation requested" arrives right here
        return {};     // commit itself succeeded -- it is NOT rolled back
    }).name("commit");

    ack = pipe.emplace([&](std::stop_token) -> std::expected<void, PipelineError>
    {
        ackRan = true;
        return {};
    }).name("ack");
    ack.succeed(commit);

    auto result = pipe.run_inline();

    CHECK(committed);
    CHECK(ackRan);  // <-- the gap: ack still runs. See the next test for the
                     //     mechanism that correctly closes this window.
    REQUIRE(result.has_value());
}

TEST_CASE("Cancel: external stop token pre-empts a successor not yet dispatched")
{
    // Same race, but exercised through the new externally-supplied
    // std::stop_token overload rather than a per-job Job::cancel() handle --
    // this is the shape Pouncer would actually use: a token tied to the
    // owning model's lifetime, not a handle to one specific successor.
    Pipeline pipe;
    std::stop_source extStop;
    bool ackRan = false;

    auto commit = pipe.emplace([&]() -> std::expected<void, PipelineError>
    {
        extStop.request_stop();  // e.g. owning model starts tearing down here
        return {};
    }).name("commit");

    auto ack = pipe.emplace([&](std::stop_token) -> std::expected<void, PipelineError>
    {
        ackRan = true;
        return {};
    }).name("ack");
    ack.succeed(commit);

    auto result = pipe.run_inline(extStop.get_token());

    CHECK_FALSE(ackRan);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCancelled);
    CHECK(pipe.status(ack) == JobStatus::kCancelled);
}

TEST_CASE("Cancel: external stop token cancels not-yet-started independent roots too")
{
    // Two independent root jobs (no dependency between them). The first
    // fires the external token; the second must not start even though it
    // has no predecessor relationship to the first -- an external token
    // scopes to the whole run, not to one DAG edge.
    Pipeline pipe;
    std::stop_source extStop;
    bool secondRan = false;

    RecordingExecutor exec; // sequential, preserves dispatch order deterministically

    auto first = pipe.emplace([&]() -> std::expected<void, PipelineError>
    {
        extStop.request_stop();
        return {};
    }).name("first");

    auto second = pipe.emplace([&](std::stop_token) -> std::expected<void, PipelineError>
    {
        secondRan = true;
        return {};
    }).name("second");
    // No edge between first/second -- both are roots. RecordingExecutor
    // dispatches roots in emplace order, so "first" still runs before
    // "second" gets a chance, deterministically.

    auto result = pipe.run(exec, extStop.get_token());

    CHECK_FALSE(secondRan);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCancelled);
    CHECK(pipe.status(second) == JobStatus::kCancelled);
}

TEST_CASE("Cancel: run() returning kTimeout is NOT the safe-to-free signal -- join_orphans() is")
{
    // Reproduces the exact ambiguity issue #1 is about: a non-cancellable
    // job (ignores its stop_token, as real blocked I/O might) that blows
    // its timeout gets a hard packaged_task cutoff. Previously that thread
    // was detach()'d -- run() would return kTimeout while the thread kept
    // running and touching whatever it closed over. This test proves the
    // two signals are now distinguishable:
    //   - run() returns promptly (bounded by the declared timeout).
    //   - has_pending_orphans() is true immediately after -- NOT safe to free.
    //   - join_orphans() blocks until the thread actually exits, at which
    //     point has_pending_orphans() is false and it IS safe to free.
    //
    // This test necessarily uses a real background thread and real wall-clock
    // timing (DesktopExecutor + a genuinely blocked, non-cancellable job) --
    // unlike the deterministic DAG-cancellation tests above, there is no way
    // to prove an orphan OS thread's lifetime without a real thread.
    auto exec = sub0pipeline::makeDesktopExecutor();
    Pipeline pipe;

    std::atomic<bool> jobStillRunning{true};
    std::atomic<bool> jobActuallyFinished{false};

    // Non-cancellable: does NOT take std::stop_token, so it cannot observe
    // request_stop() -- exactly the "UDAW job functions time out at the
    // syscall level" case the original Job::timeout() doc referred to.
    auto j = pipe.emplace([&]() -> std::expected<void, PipelineError>
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{80});
        jobStillRunning.store(false);
        jobActuallyFinished.store(true);
        return {};
    });
    j.name("blocked_io").timeout(20ms);

    auto result = pipe.run(*exec);

    CHECK_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kTimeout);
    // run() returned well before the job's 80ms sleep finished -- "stop
    // requested" (well, "timed out") but NOT "safe to free": the job
    // closure captured jobStillRunning/jobActuallyFinished by reference,
    // and the thread is still writing to them.
    CHECK(jobStillRunning.load());
    CHECK_FALSE(jobActuallyFinished.load());
    CHECK(pipe.has_pending_orphans());

    // The explicit "safe to free borrowed state" signal: blocks until the
    // orphaned thread has actually finished.
    const bool joinedSomething = pipe.join_orphans();
    CHECK(joinedSomething);
    CHECK(jobActuallyFinished.load());
    CHECK_FALSE(pipe.has_pending_orphans());
}
