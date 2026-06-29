// tests/test_rerun.cpp
//
// Re-runnability tests (epoch-based reset).

#include <sub0pipeline/sub0pipeline.hpp>
#include "test_helpers.hpp"
#include "doctest.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace sub0pipeline;

// ═══════════════════════════════════════════════════════════════════════════════
// Re-runnability (epoch-based reset)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: run() can be called multiple times")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    auto a = pipeline.emplace([&] { ++counter; }).name("A");
    auto b = pipeline.emplace([&] { ++counter; }).name("B");
    b.succeed(a);

    auto r1 = pipeline.run(exec);
    REQUIRE(r1.has_value());
    CHECK(counter == 2);
    CHECK(pipeline.status(a) == JobStatus::kDone);
    CHECK(pipeline.status(b) == JobStatus::kDone);

    // Second run -- same pipeline, counter keeps incrementing
    auto r2 = pipeline.run(exec);
    REQUIRE(r2.has_value());
    CHECK(counter == 4);
    CHECK(pipeline.status(a) == JobStatus::kDone);
    CHECK(pipeline.status(b) == JobStatus::kDone);

    // Third run
    auto r3 = pipeline.run(exec);
    REQUIRE(r3.has_value());
    CHECK(counter == 6);
}

TEST_CASE("Pipeline: re-run preserves correct execution order")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    a.precede(b, c);

    for (int i = 0; i < 3; ++i) {
        exec.clear();
        auto result = pipeline.run(exec);
        REQUIRE(result.has_value());
        CHECK(exec.order().front() == "A");
        CHECK(exec.order().size() == 3U);
    }
}

TEST_CASE("Pipeline: re-run diamond graph")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    auto a = pipeline.emplace([&] { ++counter; }).name("A");
    auto b = pipeline.emplace([&] { ++counter; }).name("B");
    auto c = pipeline.emplace([&] { ++counter; }).name("C");
    auto d = pipeline.emplace([&] { ++counter; }).name("D");
    a.precede(b, c);
    d.succeed(b, c);

    for (int run = 1; run <= 5; ++run) {
        exec.clear();
        auto result = pipeline.run(exec);
        REQUIRE(result.has_value());
        CHECK(counter == run * 4);
        CHECK(exec.order().front() == "A");
        CHECK(exec.order().back() == "D");
    }
}

TEST_CASE("Pipeline: re-run after failure resets error state")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    bool              shouldFail = true;

    auto a = pipeline.emplace([&]() -> std::expected<void, PipelineError> {
        if (shouldFail) return std::unexpected(PipelineError::kJobFailed);
        return {};
    }).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    b.succeed(a);

    // First run: A fails, B is skipped
    auto r1 = pipeline.run(exec);
    CHECK_FALSE(r1.has_value());
    CHECK(pipeline.status(a) == JobStatus::kFailed);
    CHECK(pipeline.status(b) == JobStatus::kSkipped);

    // Second run: A succeeds this time
    shouldFail = false;
    auto r2 = pipeline.run(exec);
    REQUIRE(r2.has_value());
    CHECK(pipeline.status(a) == JobStatus::kDone);
    CHECK(pipeline.status(b) == JobStatus::kDone);
}

TEST_CASE("Pipeline: re-run single job (simplest case)")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    pipeline.emplace([&] { ++counter; }).name("solo");

    for (int run = 1; run <= 3; ++run) {
        auto result = pipeline.run(exec);
        REQUIRE(result.has_value());
        CHECK(counter == run);
    }
}

TEST_CASE("Pipeline: re-run all-roots (no edges)")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    for (int i = 0; i < 5; ++i)
        pipeline.emplace([&] { ++counter; }).name("r" + std::to_string(i));

    for (int run = 1; run <= 3; ++run) {
        exec.clear();
        auto result = pipeline.run(exec);
        REQUIRE(result.has_value());
        CHECK(counter == run * 5);
        CHECK(exec.order().size() == 5U);
    }
}

TEST_CASE("Pipeline: re-run disconnected subgraphs")
{
    // Two independent chains: A->B and C->D
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    auto a = pipeline.emplace([&] { ++counter; }).name("A");
    auto b = pipeline.emplace([&] { ++counter; }).name("B");
    auto c = pipeline.emplace([&] { ++counter; }).name("C");
    auto d = pipeline.emplace([&] { ++counter; }).name("D");
    b.succeed(a);
    d.succeed(c);

    for (int run = 1; run <= 3; ++run) {
        exec.clear();
        auto result = pipeline.run(exec);
        REQUIRE(result.has_value());
        CHECK(counter == run * 4);

        // Both subgraphs must execute
        CHECK(pipeline.status(a) == JobStatus::kDone);
        CHECK(pipeline.status(b) == JobStatus::kDone);
        CHECK(pipeline.status(c) == JobStatus::kDone);
        CHECK(pipeline.status(d) == JobStatus::kDone);

        // Ordering within each chain
        const auto& order = exec.order();
        auto aPos = std::find(order.begin(), order.end(), "A") - order.begin();
        auto bPos = std::find(order.begin(), order.end(), "B") - order.begin();
        auto cPos = std::find(order.begin(), order.end(), "C") - order.begin();
        auto dPos = std::find(order.begin(), order.end(), "D") - order.begin();
        CHECK(aPos < bPos);
        CHECK(cPos < dPos);
    }
}

TEST_CASE("Pipeline: re-run deep chain (20 nodes)")
{
    constexpr int     cN = 20;
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    std::vector<Job> jobs;
    jobs.reserve(cN);
    for (int i = 0; i < cN; ++i) {
        auto j = pipeline.emplace([&] { ++counter; }).name("j" + std::to_string(i));
        if (!jobs.empty()) j.succeed(jobs.back());
        jobs.push_back(j);
    }

    for (int run = 1; run <= 3; ++run) {
        exec.clear();
        auto result = pipeline.run(exec);
        REQUIRE(result.has_value());
        CHECK(counter == run * cN);

        // Must execute in exact linear order
        const auto& order = exec.order();
        REQUIRE(order.size() == static_cast<std::size_t>(cN));
        for (int i = 0; i < cN; ++i)
            CHECK(order[static_cast<std::size_t>(i)] == "j" + std::to_string(i));
    }
}

TEST_CASE("Pipeline: re-run wide fan-out (1 root + many leaves)")
{
    constexpr int     cLeaves = 20;
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    auto root = pipeline.emplace([&] { ++counter; }).name("root");
    for (int i = 0; i < cLeaves; ++i) {
        pipeline.emplace([&] { ++counter; })
            .name("leaf_" + std::to_string(i))
            .succeed(root);
    }

    for (int run = 1; run <= 3; ++run) {
        exec.clear();
        auto result = pipeline.run(exec);
        REQUIRE(result.has_value());
        CHECK(counter == run * (1 + cLeaves));
        CHECK(exec.order().front() == "root");
        CHECK(exec.order().size() == static_cast<std::size_t>(1 + cLeaves));
    }
}

TEST_CASE("Pipeline: re-run with observer receives callbacks each run")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    b.succeed(a);

    struct CountingObserver final : IObserver {
        int starts = 0, finishes = 0;
        void onStart(std::string_view) override { ++starts; }
        void onFinish(std::string_view, JobStatus, float) override { ++finishes; }
    } obs;

    // Run 1
    auto r1 = pipeline.run(exec, &obs);
    REQUIRE(r1.has_value());
    CHECK(obs.starts == 2);
    CHECK(obs.finishes == 2);

    // Run 2 -- observer should get callbacks again
    auto r2 = pipeline.run(exec, &obs);
    REQUIRE(r2.has_value());
    CHECK(obs.starts == 4);
    CHECK(obs.finishes == 4);
}

TEST_CASE("Pipeline: re-run optional failure then success")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               callCount = 0;
    bool              shouldFail = true;

    auto a = pipeline.emplace([&]() -> std::expected<void, PipelineError> {
        ++callCount;
        if (shouldFail) return std::unexpected(PipelineError::kJobFailed);
        return {};
    }).name("A").optional();
    auto b = pipeline.emplace([&] { ++callCount; }).name("B");
    b.succeed(a);

    // Run 1: A fails (optional), B still runs
    auto r1 = pipeline.run(exec);
    REQUIRE(r1.has_value());
    CHECK(pipeline.status(a) == JobStatus::kFailed);
    CHECK(pipeline.status(b) == JobStatus::kDone);
    CHECK(callCount == 2);

    // Run 2: A succeeds, B still runs
    shouldFail = false;
    auto r2 = pipeline.run(exec);
    REQUIRE(r2.has_value());
    CHECK(pipeline.status(a) == JobStatus::kDone);
    CHECK(pipeline.status(b) == JobStatus::kDone);
    CHECK(callCount == 4);
}

TEST_CASE("Pipeline: re-run required failure cascade then recovery")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    bool              shouldFail = true;

    // A (required) -> B -> C
    auto a = pipeline.emplace([&]() -> std::expected<void, PipelineError> {
        if (shouldFail) return std::unexpected(PipelineError::kJobFailed);
        return {};
    }).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    a.precede(b);
    b.precede(c);

    // Run 1: A fails -> B,C skipped
    auto r1 = pipeline.run(exec);
    CHECK_FALSE(r1.has_value());
    CHECK(pipeline.status(a) == JobStatus::kFailed);
    CHECK(pipeline.status(b) == JobStatus::kSkipped);
    CHECK(pipeline.status(c) == JobStatus::kSkipped);

    // Run 2: A succeeds -> B,C run normally
    shouldFail = false;
    auto r2 = pipeline.run(exec);
    REQUIRE(r2.has_value());
    CHECK(pipeline.status(a) == JobStatus::kDone);
    CHECK(pipeline.status(b) == JobStatus::kDone);
    CHECK(pipeline.status(c) == JobStatus::kDone);
}

TEST_CASE("Pipeline: re-run fan-in (many predecessors -> one sink)")
{
    constexpr int     cN = 10;
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    std::vector<Job> leaves;
    for (int i = 0; i < cN; ++i)
        leaves.push_back(pipeline.emplace([&] { ++counter; }).name("l" + std::to_string(i)));

    auto sink = pipeline.emplace([&] { ++counter; }).name("sink");
    for (auto& leaf : leaves) sink.succeed(leaf);

    for (int run = 1; run <= 3; ++run) {
        exec.clear();
        auto result = pipeline.run(exec);
        REQUIRE(result.has_value());
        CHECK(counter == run * (cN + 1));
        CHECK(exec.order().back() == "sink");

        for (auto& leaf : leaves)
            CHECK(pipeline.status(leaf) == JobStatus::kDone);
        CHECK(pipeline.status(sink) == JobStatus::kDone);
    }
}
