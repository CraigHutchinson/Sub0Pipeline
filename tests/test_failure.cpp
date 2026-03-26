// tests/test_failure.cpp
//
// Tests for required/optional failure propagation and dependent-job skipping.

#include <sub0pipeline/sub0pipeline.hpp>
#include "doctest.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace sub0pipeline;

// ── Inline sequential executor ────────────────────────────────────────────────

class InlineExecutor final : public IExecutor
{
public:
    void dispatch(
        std::string_view,
        std::function<void()>  fn,
        std::function<void()>  on_complete,
        int, uint8_t, uint32_t) override
    {
        fn();
        if (on_complete) on_complete();
    }
    void wait_all() override {}
    [[nodiscard]] int concurrency() const noexcept override { return 1; }
};

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
