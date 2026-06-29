// tests/test_failure.cpp
//
// Tests for required/optional failure propagation and dependent-job skipping.

#include <sub0pipeline/sub0pipeline.hpp>
#include "test_helpers.hpp"
#include "doctest.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace sub0pipeline;

// ═══════════════════════════════════════════════════════════════════════════════
// Required job failures
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Failure: required job failure propagates from run()")
{
    Pipeline      pipeline;
    InlineExecutor exec;

    pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("fail");

    auto result = pipeline.run(exec);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
}

TEST_CASE("Failure: required failure skips dependents")
{
    Pipeline      pipeline;
    InlineExecutor exec;
    bool           dependentRan = false;

    auto req = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("required");

    auto dep = pipeline.emplace([&] { dependentRan = true; }).name("dependent");
    dep.succeed(req);

    auto result = pipeline.run(exec);
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(dependentRan);
}

TEST_CASE("Failure: failure at mid-chain skips downstream only")
{
    Pipeline      pipeline;
    InlineExecutor exec;
    std::vector<std::string> ran;

    auto a = pipeline.emplace([&] { ran.push_back("A"); }).name("A");
    auto b = pipeline.emplace([&]() -> std::expected<void, PipelineError> {
        ran.push_back("B");
        return std::unexpected(PipelineError::kJobFailed);
    }).name("B");
    auto c = pipeline.emplace([&] { ran.push_back("C"); }).name("C");

    b.succeed(a);
    c.succeed(b);

    (void)pipeline.run(exec);

    REQUIRE(ran.size() == 2U);
    CHECK(std::find(ran.begin(), ran.end(), "A") != ran.end());
    CHECK(std::find(ran.begin(), ran.end(), "B") != ran.end());
    CHECK(std::find(ran.begin(), ran.end(), "C") == ran.end());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Optional job failures
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Failure: optional job failure does not block dependents")
{
    Pipeline      pipeline;
    InlineExecutor exec;
    bool           dependentRan = false;

    auto opt = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("optional").optional();

    auto dep = pipeline.emplace([&] { dependentRan = true; }).name("dependent");
    dep.succeed(opt);

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(dependentRan);
}

TEST_CASE("Failure: optional job failure does not affect run() result")
{
    Pipeline      pipeline;
    InlineExecutor exec;

    pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("opt").optional();

    auto result = pipeline.run(exec);
    CHECK(result.has_value());
}

TEST_CASE("Failure: multiple required failures — first error returned")
{
    Pipeline      pipeline;
    InlineExecutor exec;

    pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("fail1");

    pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kTimeout);
    }).name("fail2");

    auto result = pipeline.run(exec);
    REQUIRE_FALSE(result.has_value());
    // Either error is acceptable — we just check one of the expected codes.
    CHECK((result.error() == PipelineError::kJobFailed
        || result.error() == PipelineError::kTimeout));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Complex error propagation
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: status() is kFailed for optional failed job")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    auto a = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("A").optional();
    (void)pipeline.run(exec);
    CHECK(pipeline.status(a) == JobStatus::kFailed);
}

TEST_CASE("Pipeline: required failure cascades to all subsequent roots (sequential)")
{
    // Two independent chains: A(fail)->B and C(fail)->D
    // With sequential executor: A runs first and fails, setting hasFatalFailure.
    // C (also a root) is then skipped by the fatal failure check, not executed.
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    b.succeed(a);

    auto c = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("C");
    auto d = pipeline.emplace([] {}).name("D");
    d.succeed(c);

    auto result = pipeline.run(exec);
    CHECK_FALSE(result.has_value());

    // A fails, B is skipped due to predecessor failure
    CHECK(pipeline.status(a) == JobStatus::kFailed);
    CHECK(pipeline.status(b) == JobStatus::kSkipped);
    // C and D are skipped because hasFatalFailure was already set by A
    CHECK(pipeline.status(c) == JobStatus::kSkipped);
    CHECK(pipeline.status(d) == JobStatus::kSkipped);
}

TEST_CASE("Pipeline: failure in middle of diamond")
{
    //   A -> {B(fail), C} -> D
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    auto d = pipeline.emplace([] {}).name("D");

    a.precede(b, c);
    d.succeed(b, c);

    auto result = pipeline.run(exec);
    CHECK_FALSE(result.has_value());
    CHECK(pipeline.status(a) == JobStatus::kDone);
    CHECK(pipeline.status(b) == JobStatus::kFailed);
    // D must be skipped since required predecessor B failed
    CHECK(pipeline.status(d) == JobStatus::kSkipped);
}

TEST_CASE("Pipeline: optional job in critical path -- required successor still runs")
{
    //   A (required) -> B (optional, fails) -> C (required)
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("B").optional();
    auto c = pipeline.emplace([] {}).name("C");
    a.precede(b);
    b.precede(c);

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(pipeline.status(a) == JobStatus::kDone);
    CHECK(pipeline.status(b) == JobStatus::kFailed);
    CHECK(pipeline.status(c) == JobStatus::kDone);

    // Re-run: verify C still runs after optional B fails again
    auto r2 = pipeline.run(exec);
    REQUIRE(r2.has_value());
    CHECK(pipeline.status(c) == JobStatus::kDone);
}

TEST_CASE("Pipeline: double-diamond with failure in first diamond, recovery on re-run")
{
    //   A -> {B(fail), C} -> D -> {E, F} -> G
    Pipeline          pipeline;
    RecordingExecutor exec;
    bool              shouldFail = true;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([&]() -> std::expected<void, PipelineError> {
        if (shouldFail) return std::unexpected(PipelineError::kJobFailed);
        return {};
    }).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    auto d = pipeline.emplace([] {}).name("D");
    auto e = pipeline.emplace([] {}).name("E");
    auto f = pipeline.emplace([] {}).name("F");
    auto g = pipeline.emplace([] {}).name("G");

    a.precede(b, c);
    d.succeed(b, c);
    d.precede(e, f);
    g.succeed(e, f);

    // Run 1: B fails -> D,E,F,G all skipped
    auto r1 = pipeline.run(exec);
    CHECK_FALSE(r1.has_value());
    CHECK(pipeline.status(b) == JobStatus::kFailed);
    CHECK(pipeline.status(d) == JobStatus::kSkipped);
    CHECK(pipeline.status(g) == JobStatus::kSkipped);

    // Run 2: B succeeds -> entire graph completes
    shouldFail = false;
    exec.clear();
    auto r2 = pipeline.run(exec);
    REQUIRE(r2.has_value());

    for (auto j : {a, b, c, d, e, f, g})
        CHECK(pipeline.status(j) == JobStatus::kDone);
    CHECK(exec.order().front() == "A");
    CHECK(exec.order().back() == "G");
}
