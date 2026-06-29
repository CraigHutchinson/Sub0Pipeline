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
