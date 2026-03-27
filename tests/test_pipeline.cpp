// tests/test_pipeline.cpp
//
// Core DAG construction and execution ordering tests.
// Uses a RecordingExecutor (sequential, inline) for deterministic results.

#include <sub0pipeline/sub0pipeline.hpp>
#include "doctest.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace sub0pipeline;
using namespace std::chrono_literals;

// ── Test helpers ──────────────────────────────────────────────────────────────

/// Sequential executor that records dispatch order before execution.
class RecordingExecutor final : public IExecutor
{
public:
    void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           /*core*/,
        uint8_t                       /*priority*/,
        uint32_t                      /*stack_bytes*/) override
    {
        order_.emplace_back(name);  // capture BEFORE execution
        fn();
        if (onComplete) onComplete();
    }

    void wait_all() override {}
    [[nodiscard]] int concurrency() const noexcept override { return 1; }

    [[nodiscard]] const std::vector<std::string>& order() const { return order_; }
    void clear() { order_.clear(); }

private:
    std::vector<std::string> order_;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: empty pipeline runs successfully")
{
    Pipeline       pipeline;
    RecordingExecutor exec;

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(pipeline.size() == 0U);
}

TEST_CASE("Pipeline: single void job runs")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    bool              ran = false;

    pipeline.emplace([&] { ran = true; }).name("single");
    auto result = pipeline.run(exec);

    REQUIRE(result.has_value());
    CHECK(ran);
}

TEST_CASE("Pipeline: single expected-returning job runs")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    pipeline.emplace([]() -> std::expected<void, PipelineError> { return {}; }).name("ok");
    auto result = pipeline.run(exec);

    REQUIRE(result.has_value());
}

TEST_CASE("Pipeline: job handle properties")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto j = pipeline.emplace([] {}).name("test").timeout(5000ms).core(1).priority(10);
    CHECK(j.valid());
    CHECK(pipeline.name(j) == "test");
}

TEST_CASE("Pipeline: default job handle is invalid")
{
    Job j;
    CHECK_FALSE(j.valid());
    CHECK_FALSE(static_cast<bool>(j));
}

TEST_CASE("Pipeline: size tracks emplace count")
{
    Pipeline pipeline;
    CHECK(pipeline.size() == 0U);
    pipeline.emplace([] {});
    CHECK(pipeline.size() == 1U);
    pipeline.emplace([] {});
    pipeline.emplace([] {});
    CHECK(pipeline.size() == 3U);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Dependency ordering
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: linear chain runs in order A→B→C")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    b.succeed(a);
    c.succeed(b);

    (void)pipeline.run(exec);

    const auto& order = exec.order();
    REQUIRE(order.size() == 3U);
    CHECK(order[0] == "A");
    CHECK(order[1] == "B");
    CHECK(order[2] == "C");
}

TEST_CASE("Pipeline: diamond dependency — A first, D last")
{
    //    A
    //   / \
    //  B   C
    //   \ /
    //    D
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    auto d = pipeline.emplace([] {}).name("D");

    a.precede(b, c);
    d.succeed(b, c);

    (void)pipeline.run(exec);

    const auto& order = exec.order();
    REQUIRE(order.size() == 4U);
    CHECK(order[0] == "A");
    CHECK(order[3] == "D");
}

TEST_CASE("Pipeline: independent jobs all run")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    bool              aRan = false, bRan = false, cRan = false;

    pipeline.emplace([&] { aRan = true; }).name("A");
    pipeline.emplace([&] { bRan = true; }).name("B");
    pipeline.emplace([&] { cRan = true; }).name("C");

    (void)pipeline.run(exec);

    CHECK(aRan);
    CHECK(bRan);
    CHECK(cRan);
}

TEST_CASE("Pipeline: precede chaining — A runs first")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    a.precede(b).precede(c);   // A → B, A → C (fan-out from A)

    (void)pipeline.run(exec);
    CHECK(exec.order()[0] == "A");
}

TEST_CASE("Pipeline: succeed chaining — C after A and B")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    c.succeed(a, b);

    (void)pipeline.run(exec);

    const auto& order = exec.order();
    const auto  cPos  = std::find(order.begin(), order.end(), "C") - order.begin();
    const auto  aPos  = std::find(order.begin(), order.end(), "A") - order.begin();
    const auto  bPos  = std::find(order.begin(), order.end(), "B") - order.begin();
    CHECK(cPos > aPos);
    CHECK(cPos > bPos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stress / large DAGs
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: large linear chain N=100")
{
    constexpr int cN = 100;
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    std::vector<Job> jobs;
    jobs.reserve(cN);
    for (int i = 0; i < cN; ++i) {
        auto j = pipeline.emplace([&] { ++counter; }).name("job_" + std::to_string(i));
        if (!jobs.empty()) j.succeed(jobs.back());
        jobs.push_back(j);
    }

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(counter == cN);
}

TEST_CASE("Pipeline: wide fan-out N=50")
{
    constexpr int cN = 50;
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    auto root = pipeline.emplace([] {}).name("root");
    for (int i = 0; i < cN; ++i) {
        pipeline.emplace([&] { ++counter; })
            .name("leaf_" + std::to_string(i))
            .succeed(root);
    }

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(counter == cN);
}

TEST_CASE("Pipeline: wide fan-in N=50")
{
    constexpr int cN = 50;
    Pipeline          pipeline;
    RecordingExecutor exec;
    bool              sinkRan = false;

    std::vector<Job> leaves;
    leaves.reserve(cN);
    for (int i = 0; i < cN; ++i) {
        leaves.push_back(pipeline.emplace([] {}).name("leaf_" + std::to_string(i)));
    }

    auto sink = pipeline.emplace([&] { sinkRan = true; }).name("sink");
    for (auto& leaf : leaves) sink.succeed(leaf);

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(sinkRan);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Diagnostics
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: dump_text does not crash")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    b.succeed(a);
    // Output goes to stdout — just verify no crash.
    pipeline.dump_text();
}

// ═══════════════════════════════════════════════════════════════════════════════
// status() queries
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: status() is kPending before run()")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    CHECK(pipeline.status(a) == JobStatus::kPending);
}

TEST_CASE("Pipeline: status() is kDone after successful run()")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    b.succeed(a);
    (void)pipeline.run(exec);
    CHECK(pipeline.status(a) == JobStatus::kDone);
    CHECK(pipeline.status(b) == JobStatus::kDone);
}

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

TEST_CASE("Pipeline: invalid job handle returns kPending from status()")
{
    Pipeline pipeline;
    Job invalid;
    CHECK(pipeline.status(invalid) == JobStatus::kPending);
}

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

    // Second run — same pipeline, counter keeps incrementing
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

// ═══════════════════════════════════════════════════════════════════════════════
// JobGroup / parallel()
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("parallel: creates JobGroup with correct member count")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");

    auto group = parallel(a, b, c);
    CHECK(group.jobs().size() == 3U);
}

TEST_CASE("JobGroup::succeed wires all members after a single job")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto root = pipeline.emplace([] {}).name("root");
    auto a    = pipeline.emplace([] {}).name("A");
    auto b    = pipeline.emplace([] {}).name("B");

    auto group = parallel(a, b);
    group.succeed(root);  // both A and B depend on root

    (void)pipeline.run(exec);
    const auto& order = exec.order();
    auto rootPos = std::find(order.begin(), order.end(), "root") - order.begin();
    auto aPos    = std::find(order.begin(), order.end(), "A")    - order.begin();
    auto bPos    = std::find(order.begin(), order.end(), "B")    - order.begin();
    CHECK(aPos > rootPos);
    CHECK(bPos > rootPos);
}

TEST_CASE("JobGroup::precede wires all members before a single job")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a    = pipeline.emplace([] {}).name("A");
    auto b    = pipeline.emplace([] {}).name("B");
    auto sink = pipeline.emplace([] {}).name("sink");

    auto group = parallel(a, b);
    group.precede(sink);  // sink depends on both A and B

    (void)pipeline.run(exec);
    const auto& order = exec.order();
    auto sinkPos = std::find(order.begin(), order.end(), "sink") - order.begin();
    auto aPos    = std::find(order.begin(), order.end(), "A")    - order.begin();
    auto bPos    = std::find(order.begin(), order.end(), "B")    - order.begin();
    CHECK(sinkPos > aPos);
    CHECK(sinkPos > bPos);
}

TEST_CASE("Job::succeed(JobGroup) wires job after all group members")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a    = pipeline.emplace([] {}).name("A");
    auto b    = pipeline.emplace([] {}).name("B");
    auto sink = pipeline.emplace([] {}).name("sink");

    auto group = parallel(a, b);
    sink.succeed(group);

    (void)pipeline.run(exec);
    const auto& order = exec.order();
    auto sinkPos = std::find(order.begin(), order.end(), "sink") - order.begin();
    auto aPos    = std::find(order.begin(), order.end(), "A")    - order.begin();
    auto bPos    = std::find(order.begin(), order.end(), "B")    - order.begin();
    CHECK(sinkPos > aPos);
    CHECK(sinkPos > bPos);
}

TEST_CASE("Job::precede(JobGroup) wires job before all group members")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto root = pipeline.emplace([] {}).name("root");
    auto a    = pipeline.emplace([] {}).name("A");
    auto b    = pipeline.emplace([] {}).name("B");

    auto group = parallel(a, b);
    root.precede(group);

    (void)pipeline.run(exec);
    const auto& order = exec.order();
    auto rootPos = std::find(order.begin(), order.end(), "root") - order.begin();
    auto aPos    = std::find(order.begin(), order.end(), "A")    - order.begin();
    auto bPos    = std::find(order.begin(), order.end(), "B")    - order.begin();
    CHECK(aPos > rootPos);
    CHECK(bPos > rootPos);
}

TEST_CASE("Job::pipeline() returns owning pipeline")
{
    Pipeline pipeline;
    auto a = pipeline.emplace([] {}).name("A");
    CHECK(a.pipeline() == &pipeline);
}

TEST_CASE("Job::pipeline() returns nullptr for default job")
{
    Job j;
    CHECK(j.pipeline() == nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Generic emplace(Spec)
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
struct TestSpec
{
    std::string nameStr;
    Job build(Pipeline& p) { return p.emplace([] {}).name(nameStr); }
};
} // namespace

TEST_CASE("Pipeline: emplace(Spec) accepts buildable type")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto j = pipeline.emplace(TestSpec{"hello"});
    CHECK(j.valid());
    CHECK(pipeline.name(j) == "hello");

    (void)pipeline.run(exec);
    CHECK(pipeline.status(j) == JobStatus::kDone);
}

TEST_CASE("Pipeline: emplace(Spec, Spec) returns tuple for structured bindings")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto [a, b, c] = pipeline.emplace(
        TestSpec{"A"},
        TestSpec{"B"},
        TestSpec{"C"}
    );

    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(c.valid());
    CHECK(pipeline.name(a) == "A");
    CHECK(pipeline.name(b) == "B");
    CHECK(pipeline.name(c) == "C");
    CHECK(pipeline.size() == 3U);
}
