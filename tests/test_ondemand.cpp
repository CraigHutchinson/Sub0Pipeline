// tests/test_ondemand.cpp
//
// On-demand job tests (run_inline and add_on_demand / arm / trigger).

#include <sub0pipeline/sub0pipeline.hpp>
#include "test_helpers.hpp"
#include "doctest.h"

using namespace sub0pipeline;

// ═══════════════════════════════════════════════════════════════════════════════
// run_inline
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("run_inline: executes pipeline synchronously without an explicit executor")
{
    Pipeline pipe;
    int count = 0;
    pipe.emplace([&]{ ++count; }).name("a");
    pipe.emplace([&]{ ++count; }).name("b");

    auto result = pipe.run_inline();

    CHECK(result.has_value());
    CHECK(count == 2);
}

TEST_CASE("run_inline: failure propagates correctly")
{
    Pipeline pipe;
    pipe.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("fail");

    auto result = pipe.run_inline();
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// OnDemand
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("OnDemand: on-demand job does not run during normal run()")
{
    RecordingExecutor exec;
    Pipeline pipe;

    int callCount = 0;
    auto od = pipe.add_on_demand([&]() -> std::expected<void, PipelineError> {
        ++callCount;
        return {};
    });
    od.name("fetch");

    // Normal run() must not execute on-demand jobs.
    (void)pipe.run(exec);

    CHECK(callCount == 0);
    CHECK_FALSE(exec.order().end() != std::find(exec.order().begin(), exec.order().end(), "fetch"));
}

TEST_CASE("OnDemand: trigger() dispatches job via armed executor")
{
    RecordingExecutor exec;
    Pipeline pipe;

    int callCount = 0;
    auto od = pipe.add_on_demand([&]() -> std::expected<void, PipelineError> {
        ++callCount;
        return {};
    });
    od.name("fetch");

    pipe.arm(exec);
    auto result = pipe.trigger(od);

    CHECK(result.has_value());
    CHECK(callCount == 1);
}

TEST_CASE("OnDemand: trigger() can be called multiple times independently")
{
    RecordingExecutor exec;
    Pipeline pipe;

    int callCount = 0;
    auto od = pipe.add_on_demand([&]() -> std::expected<void, PipelineError> {
        ++callCount;
        return {};
    });

    pipe.arm(exec);
    (void)pipe.trigger(od);
    (void)pipe.trigger(od);
    (void)pipe.trigger(od);

    CHECK(callCount == 3);
}

TEST_CASE("OnDemand: trigger() without arm() returns kNotArmed")
{
    RecordingExecutor exec;
    Pipeline pipe;

    auto od = pipe.add_on_demand([]() -> std::expected<void, PipelineError> {
        return {};
    });

    auto result = pipe.trigger(od);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kNotArmed);
}

TEST_CASE("OnDemand: trigger() on a non-on-demand job returns kNotOnDemand")
{
    RecordingExecutor exec;
    Pipeline pipe;

    auto regular = pipe.emplace([]() -> std::expected<void, PipelineError> {
        return {};
    });
    regular.name("regular");

    pipe.arm(exec);
    auto result = pipe.trigger(regular);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kNotOnDemand);
}

TEST_CASE("OnDemand: normal jobs and on-demand jobs coexist -- only normal jobs run via run()")
{
    RecordingExecutor exec;
    Pipeline pipe;

    int normalCount   = 0;
    int onDemandCount = 0;

    pipe.emplace([&]() -> std::expected<void, PipelineError> {
        ++normalCount; return {};
    }).name("normal");

    auto od = pipe.add_on_demand([&]() -> std::expected<void, PipelineError> {
        ++onDemandCount; return {};
    });
    od.name("on_demand");

    (void)pipe.run(exec);

    CHECK(normalCount   == 1);
    CHECK(onDemandCount == 0);

    pipe.arm(exec);
    (void)pipe.trigger(od);

    CHECK(onDemandCount == 1);
    CHECK(normalCount   == 1);
}
