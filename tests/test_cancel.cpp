// tests/test_cancel.cpp
//
// Cancellation and timeout tests.

#include <sub0pipeline/sub0pipeline.hpp>
#include "test_helpers.hpp"
#include "doctest.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <latch>
#include <barrier>
#include <mutex>
#include <thread>

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
    // cancel() is designed for mid-run concurrent use; Run initialization creates a
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
    // Run initialization creates a fresh stop_source per epoch so that cancellation
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
    j.cancel();  // fired before run() -- will be reset by run initialization

    auto result = pipe.run(exec);
    CHECK(result.has_value());  // cancel had no effect
    CHECK(ran == 1);
}

TEST_CASE("Cancel: pre-run cancellation also resets for plain functions")
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
        std::mutex mutex;
        std::condition_variable_any ready;
        std::unique_lock lock{mutex};
        ready.wait(lock, st, [] { return false; });
        return std::unexpected(PipelineError::kCancelled);
    });
    j.name("slow_fetch").timeout(50ms);

    auto result = pipe.run(*exec);
    CHECK_FALSE(result.has_value());
    // Either kCancelled (cooperative exit via stop_token) or kTimeout (hard cutoff)
    CHECK((result.error() == PipelineError::kCancelled
        || result.error() == PipelineError::kTimeout));
}

// A firmware device validates records, commits durably, then acknowledges them.
TEST_CASE("Cancel: successor cancellation survives run initialization")
{
    Pipeline pipe;
    bool committed = false;
    bool ackRan = false;
    Job ack;
    auto commit = pipe.emplace([&] { committed = true; ack.cancel(); });
    ack = pipe.emplace([&] { ackRan = true; });
    ack.succeed(commit);
    auto result = pipe.run_inline();
    CHECK(committed);
    CHECK_FALSE(ackRan);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCancelled);
}

TEST_CASE("Cancel: queued roots and successors observe external stop")
{
    for (bool successor : {false, true}) {
        Pipeline pipe;
        QueuedExecutor executor;
        std::stop_source stop;
        bool ackRan = false;
        auto commit = pipe.emplace([&] { stop.request_stop(); });
        auto ack = pipe.emplace([&] { ackRan = true; });
        if (successor) ack.succeed(commit);
        auto result = pipe.run(executor, stop.get_token());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == PipelineError::kCancelled);
        CHECK_FALSE(ackRan);
    }
}

TEST_CASE("Cancel: every edge suppresses later device stages")
{
    for (int edge = 0; edge != 3; ++edge) {
        Pipeline pipe;
        std::stop_source stop;
        int calls = 0;
        Job previous;
        for (int stage = 0; stage != 4; ++stage) {
            auto job = pipe.emplace([&, stage] {
                ++calls;
                if (stage == edge) stop.request_stop();
            });
            if (previous.valid()) job.succeed(previous);
            previous = job;
        }
        auto result = pipe.run_inline(stop.get_token());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == PipelineError::kCancelled);
        CHECK(calls == edge + 1);
    }
}

TEST_CASE("Cancel: optional cancellation is fatal but ordinary optional failure is not")
{
    for (auto error : {PipelineError::kCancelled, PipelineError::kJobFailed}) {
        Pipeline pipe;
        bool successorRan = false;
        auto first = pipe.emplace([=]() -> std::expected<void, PipelineError> {
            return std::unexpected(error);
        }).optional();
        auto next = pipe.emplace([&] { successorRan = true; }).succeed(first);
        auto result = pipe.run_inline();
        CHECK(result.has_value() == (error == PipelineError::kJobFailed));
        CHECK(successorRan == (error == PipelineError::kJobFailed));
        CHECK(pipe.status(next) == (successorRan ? JobStatus::kDone : JobStatus::kSkipped));
    }
}

TEST_CASE("Cancel: external request releases an in-flight cooperative wait")
{
    Pipeline pipe;
    std::stop_source stop;
    std::latch entered{1};
    auto job = pipe.emplace([&](std::stop_token token) -> std::expected<void, PipelineError> {
        std::mutex mutex;
        std::condition_variable_any ready;
        std::unique_lock lock{mutex};
        entered.count_down();
        ready.wait(lock, token, [] { return false; });
        return std::unexpected(PipelineError::kCancelled);
    });
    std::expected<void, PipelineError> result;
    std::jthread runner{[&] { result = pipe.run_inline(stop.get_token()); }};
    entered.wait();
    stop.request_stop();
    runner.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCancelled);
    CHECK(pipe.status(job) == JobStatus::kCancelled);
}

TEST_CASE("Cancel: failed commit never acknowledges")
{
    Pipeline pipe;
    bool ackRan = false;
    auto commit = pipe.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    });
    auto ack = pipe.emplace([&] { ackRan = true; }).succeed(commit);
    CHECK_FALSE(pipe.run_inline().has_value());
    CHECK_FALSE(ackRan);
    CHECK(pipe.status(ack) == JobStatus::kSkipped);
}

TEST_CASE("Cancel: queued on-demand execution uses the same cancellation gate")
{
    Pipeline pipe;
    QueuedExecutor executor;
    bool ran = false;
    auto job = pipe.add_on_demand([&]() -> std::expected<void, PipelineError> {
        ran = true;
        return {};
    });
    pipe.arm(executor);
    REQUIRE(pipe.trigger(job).has_value());
    job.cancel();
    executor.wait_all();
    CHECK_FALSE(ran);
    CHECK(pipe.status(job) == JobStatus::kCancelled);
}

TEST_CASE("Timeout: joining retains the pending signal until borrowed state is released")
{
    std::latch release{1};
    std::atomic<bool> finished{false};
    Pipeline pipe;
    auto job = pipe.emplace([&] {
        release.wait();
        // A joiner must not clear the pending signal before this access ends.
        CHECK(pipe.has_pending_orphans());
        finished = true;
    }).timeout(0ms);
    auto result = pipe.run_inline();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kTimeout);
    CHECK(pipe.has_pending_orphans());
    CHECK_FALSE(finished.load());
    std::jthread joiner{[&] { CHECK(pipe.join_orphans()); }};
    release.count_down();
    joiner.join();
    CHECK(finished.load());
    CHECK_FALSE(pipe.has_pending_orphans());
    CHECK_FALSE(pipe.join_orphans());
}

TEST_CASE("Cancel: owner teardown joins before destroying borrowed members")
{
    struct Device {
        std::latch entered{1};
        std::stop_source stop;
        int record = 42;
        Pipeline pipe;
        std::jthread runner;
        Device() {
            (void)pipe.emplace([this](std::stop_token token) -> std::expected<void, PipelineError> {
                std::mutex mutex;
                std::condition_variable_any ready;
                std::unique_lock lock{mutex};
                entered.count_down();
                ready.wait(lock, token, [] { return false; });
                CHECK(record == 42);
                return std::unexpected(PipelineError::kCancelled);
            });
            runner = std::jthread{[this] { (void)pipe.run_inline(stop.get_token()); }};
            entered.wait();
        }
        ~Device() {
            stop.request_stop();
            runner.join();
            pipe.join_orphans();
        }
    };
    auto device = std::make_unique<Device>();
    device.reset();
}

TEST_CASE("Run: reentrant execution is rejected")
{
    Pipeline pipe;
    (void)pipe.emplace([&] {
        auto nested = pipe.run_inline();
        REQUIRE_FALSE(nested.has_value());
        CHECK(nested.error() == PipelineError::kBusy);
    });
    CHECK(pipe.run_inline().has_value());
}

TEST_CASE("Cancel: fan-in stays skipped and a fresh run can retry")
{
    Pipeline pipe;
    QueuedExecutor executor;
    std::stop_source stop;
    int acknowledgements = 0;
    auto decode = pipe.emplace([] {});
    auto commit = pipe.emplace([&] { stop.request_stop(); });
    auto ack = pipe.emplace([&] { ++acknowledgements; });
    ack.succeed(decode).succeed(commit);
    auto cancelled = pipe.run(executor, stop.get_token());
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(acknowledgements == 0);
    CHECK(pipe.status(ack) == JobStatus::kCancelled);
    CHECK(pipe.run(executor).has_value());
    CHECK(acknowledgements == 1);
}

TEST_CASE("Timeout: successful cooperative work does not wait for its deadline")
{
    Pipeline pipe;
    // Completion must interrupt the watchdog, not sleep for this duration.
    (void)pipe.emplace([](std::stop_token) -> std::expected<void, PipelineError> {
        return {};
    }).timeout(24h);
    CHECK(pipe.run_inline().has_value());
}

TEST_CASE("Failure: concurrent shared descendants finish exactly once before return")
{
    Pipeline pipe;
    auto executor = makeDesktopExecutor();
    std::barrier failures{2};
    std::latch observerEntered{1}, releaseObserver{1};
    std::atomic<int> skipped{0};
    std::atomic<bool> returned{false};
    struct Observer final : IObserver {
        Pipeline& pipe;
        std::atomic<int>& skipped;
        std::latch& entered;
        std::latch& release;
        Observer(Pipeline& p, std::atomic<int>& n, std::latch& e, std::latch& r)
            : pipe{p}, skipped{n}, entered{e}, release{r} {}
        void onStart(std::string_view) override {}
        void onFinish(std::string_view, JobStatus status, float) override {
            if (status != JobStatus::kSkipped) return;
            CHECK(pipe.validate().has_value());
            if (skipped.fetch_add(1) == 0) { entered.count_down(); release.wait(); }
        }
    } observer{pipe, skipped, observerEntered, releaseObserver};
    auto fail = [&]() -> std::expected<void, PipelineError> {
        failures.arrive_and_wait();
        return std::unexpected(PipelineError::kJobFailed);
    };
    auto a = pipe.emplace(fail);
    auto b = pipe.emplace(fail);
    auto sink = pipe.emplace([] { CHECK(false); });
    for (int i = 0; i < 64; ++i) {
        auto child = pipe.emplace([] { CHECK(false); });
        child.succeed(a).succeed(b).precede(sink);
    }
    std::jthread runner{[&] {
        CHECK_FALSE(pipe.run(*executor, &observer).has_value());
        returned = true;
    }};
    observerEntered.wait();
    CHECK_FALSE(returned.load());
    releaseObserver.count_down();
    runner.join();
    CHECK(skipped.load() == 65);
    CHECK(pipe.status(sink) == JobStatus::kSkipped);
    // A second failure run must reuse storage without keeping prior queue state.
    CHECK_FALSE(pipe.run(*executor, &observer).has_value());
    CHECK(skipped.load() == 130);
}
