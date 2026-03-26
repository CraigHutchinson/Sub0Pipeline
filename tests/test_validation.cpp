// tests/test_validation.cpp
//
// DAG integrity checks: cycle detection, self-loops, valid topologies.

#include <sub0pipeline/sub0pipeline.hpp>
#include "doctest.h"

using namespace sub0pipeline;

TEST_CASE("Validation: empty pipeline is valid")
{
    Pipeline pipeline;
    auto result = pipeline.validate();
    REQUIRE(result.has_value());
}

TEST_CASE("Validation: single job is valid")
{
    Pipeline pipeline;
    pipeline.emplace([] {}).name("A");
    auto result = pipeline.validate();
    REQUIRE(result.has_value());
}

TEST_CASE("Validation: valid linear chain A→B→C")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    a.precede(b);
    b.precede(c);
    auto result = pipeline.validate();
    REQUIRE(result.has_value());
}

TEST_CASE("Validation: valid diamond DAG")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    auto d = pipeline.emplace([] {}).name("D");
    a.precede(b, c);
    d.succeed(b, c);
    auto result = pipeline.validate();
    REQUIRE(result.has_value());
}

TEST_CASE("Validation: cyclic dependency A→B→A detected")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    a.precede(b);
    b.precede(a);  // cycle

    auto result = pipeline.validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCyclicDependency);
}

TEST_CASE("Validation: self-loop A.succeed(A) detected")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    a.succeed(a);  // self-loop

    auto result = pipeline.validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCyclicDependency);
}

TEST_CASE("Validation: longer cycle A→B→C→A detected")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    a.precede(b);
    b.precede(c);
    c.precede(a);  // cycle

    auto result = pipeline.validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCyclicDependency);
}

TEST_CASE("Validation: run() calls validate() implicitly")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    a.precede(b);
    b.precede(a);  // cycle

    struct NullExecutor final : public IExecutor
    {
        void dispatch(std::string_view, std::function<void()>, std::function<void()>,
                      int, uint8_t, uint32_t) override {}
        void wait_all() override {}
        [[nodiscard]] int concurrency() const noexcept override { return 1; }
    } exec;

    auto result = pipeline.run(exec);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCyclicDependency);
}
