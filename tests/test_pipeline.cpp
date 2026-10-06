// tests/test_pipeline.cpp
//
// Core DAG construction and execution ordering tests.
// Uses a RecordingExecutor (sequential, inline) for deterministic results.

#include "sub0pipeline/sub0pipeline.hpp"
#include "test_helpers.hpp"
#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <latch>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace sub0pipeline;
using namespace std::chrono_literals;

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
    (void)pipeline.emplace([] {});
    CHECK(pipeline.size() == 1U);
    (void)pipeline.emplace([] {});
    (void)pipeline.emplace([] {});
    CHECK(pipeline.size() == 3U);
}

TEST_CASE("Pipeline: reserve changes no behavior, before or after jobs exist")
{
    RecordingExecutor exec;
    Pipeline pipeline;
    pipeline.reserve(8);
    CHECK(pipeline.size() == 0U);

    int ran = 0;
    auto first = pipeline.emplace([&] { ++ran; }).name("first");
    pipeline.reserve(64);   // may move existing jobs; handles and names must survive
    auto second = pipeline.emplace([&] { ++ran; });
    second.succeed(first);

    CHECK(pipeline.size() == 2U);
    CHECK(pipeline.name(first) == "first");
    CHECK(pipeline.run(exec).has_value());
    CHECK(ran == 2);

    Pipeline moved = std::move(pipeline);
    pipeline.reserve(4);    // a moved-from pipeline is empty but usable
    CHECK(pipeline.size() == 0U);
    CHECK(moved.size() == 2U);
}

TEST_CASE("Pipeline: every plain callable form is accepted and runs")
{
    RecordingExecutor exec;
    Pipeline pipeline;
    int ran = 0;

    struct Counter
    {
        int* count;
        void operator()() { ++*count; }             // non-const call operator
    };
    std::function<void()> erasedVoid = [&] { ++ran; };
    std::function<std::expected<void, PipelineError>()> erasedExpected =
        [&]() -> std::expected<void, PipelineError> { ++ran; return {}; };

    (void)pipeline.emplace([&] { ++ran; });
    (void)pipeline.emplace([&]() -> std::expected<void, PipelineError> { ++ran; return {}; });
    (void)pipeline.emplace(Counter{&ran});
    (void)pipeline.emplace(erasedVoid);
    (void)pipeline.emplace(erasedExpected);

    CHECK(pipeline.run(exec).has_value());
    CHECK(ran == 5);

    auto failing = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    });
    auto result = pipeline.run(exec);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
    CHECK(pipeline.status(failing) == JobStatus::kFailed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Dependency ordering
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: linear chain runs in order A->B->C")
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

TEST_CASE("Pipeline: diamond dependency -- A first, D last")
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

TEST_CASE("Pipeline: precede chaining -- A runs first")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    a.precede(b).precede(c);   // A -> B, A -> C (fan-out from A)

    (void)pipeline.run(exec);
    CHECK(exec.order()[0] == "A");
}

TEST_CASE("Pipeline: succeed chaining -- C after A and B")
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
    for (int i = 0; i < cN; ++i)
    {
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
    for (int i = 0; i < cN; ++i)
    {
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
    for (int i = 0; i < cN; ++i)
    {
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

TEST_CASE("Pipeline: dump_text writes the graph to a caller-selected stream")
{
    Pipeline pipeline;
    auto root = pipeline.emplace([] {}).name("root");
    auto left = pipeline.emplace([] {}).name("left");
    auto right = pipeline.emplace([] {}).name("right");
    left.succeed(root);
    right.succeed(root);

    std::ostringstream output;
    pipeline.dump_text(output);

    CHECK(output.str() ==
          "Pipeline DAG (3 jobs):\n"
          "  [0] root (predecessors: 0) -> ( left right)\n"
          "  [1] left (predecessors: 1) -> ()\n"
          "  [2] right (predecessors: 1) -> ()\n");
}

TEST_CASE("Pipeline: rejects nodes outside the successor index range")
{
    Pipeline pipeline;
    for (uint32_t i = 0; i <= std::numeric_limits<uint16_t>::max(); ++i)
        (void)pipeline.emplace([] {});

    CHECK(pipeline.size() == 65536U);
    CHECK_THROWS((void)pipeline.emplace([] {}));
    CHECK(pipeline.size() == 65536U);
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

TEST_CASE("Pipeline: invalid job handle returns kPending from status()")
{
    Pipeline pipeline;
    Job invalid;
    CHECK(pipeline.status(invalid) == JobStatus::kPending);
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

TEST_CASE("Job::id() is the identifier observers and JobId queries use")
{
    struct Recorder final : IObserver
    {
        std::vector<JobId> started;
        void onJobStart(RunId, JobId id, std::string_view) override { started.push_back(id); }
    } recorder;

    RecordingExecutor exec;
    Pipeline pipeline;
    auto first  = pipeline.emplace([] {}).name("first");
    auto second = pipeline.emplace([] {}).name("second");
    second.succeed(first);

    CHECK(first.id() != second.id());
    CHECK(pipeline.name(first.id()) == "first");
    CHECK(pipeline.name(second.id()) == "second");
    REQUIRE(pipeline.successors(first.id()).size() == 1U);
    CHECK((*pipeline.successors(first.id()).begin()).id == second.id());

    REQUIRE(pipeline.run(exec, &recorder).has_value());
    CHECK(recorder.started == std::vector<JobId>{first.id(), second.id()});
}

TEST_CASE("Job: handles from different pipelines are never equal")
{
    Pipeline one;
    Pipeline two;
    auto a = one.emplace([] {});
    auto b = two.emplace([] {});

    CHECK(a.id() == b.id());        // both are the first job of their pipeline
    CHECK_FALSE(a == b);
    CHECK(a == a);
    CHECK(Job{} == Job{});
}

TEST_CASE("Job::statusText is readable by id, from a snapshot and from an observer")
{
    struct Display final : IObserver
    {
        const Pipeline* pipeline{nullptr};
        std::vector<std::string> shown;
        void onJobStart(RunId, JobId id, std::string_view) override
        {
            shown.emplace_back(pipeline->statusText(id));
        }
    } display;

    RecordingExecutor exec;
    Pipeline pipeline;
    display.pipeline = &pipeline;
    auto load  = pipeline.emplace([] {}).name("load").statusText("Loading settings");
    auto plain = pipeline.emplace([] {}).name("plain");
    plain.succeed(load);

    CHECK(pipeline.statusText(load.id()) == "Loading settings");
    CHECK(pipeline.statusText(plain.id()).empty());
    CHECK(pipeline.statusText(JobId{999}).empty());

    const auto snapshot = pipeline.snapshot();
    REQUIRE(snapshot.size() == 2U);
    CHECK(snapshot[0].statusText == "Loading settings");
    CHECK(snapshot[1].statusText.empty());

    REQUIRE(pipeline.run(exec, &display).has_value());
    CHECK(display.shown == std::vector<std::string>{"Loading settings", ""});

    load.statusText(nullptr);
    CHECK(pipeline.statusText(load.id()).empty());
}

TEST_CASE("Pipeline: Job handles stay valid when the Pipeline is moved")
{
    RecordingExecutor exec;
    Pipeline source;
    int ran = 0;
    auto first  = source.emplace([&] { ++ran; }).name("first");
    auto second = source.emplace([&] { ++ran; });

    Pipeline moved = std::move(source);
    CHECK(first.pipeline() == &moved);
    CHECK(source.size() == 0U);

    // The handles taken before the move keep building and querying the graph.
    second.name("second").succeed(first);
    CHECK(moved.name(second) == "second");
    REQUIRE(moved.run(exec).has_value());
    CHECK(ran == 2);
    CHECK(moved.status(first) == JobStatus::kDone);
    CHECK(exec.order() == std::vector<std::string>{"first", "second"});

    // They belong to the destination now, not to the pipeline they came from.
    CHECK(source.status(first) == JobStatus::kPending);
    CHECK(source.name(first).empty());

    Pipeline assigned;
    assigned = std::move(moved);
    CHECK(first.pipeline() == &assigned);
    CHECK(assigned.status(second) == JobStatus::kDone);
}

TEST_CASE("Job::pipeline() returns nullptr for default job")
{
    Job j;
    CHECK(j.pipeline() == nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Generic emplace(Spec)
// ═══════════════════════════════════════════════════════════════════════════════

namespace
{
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

// ═══════════════════════════════════════════════════════════════════════════════
// Complex DAG topologies
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: double-diamond (two diamonds joined)")
{
    //   A -> {B,C} -> D -> {E,F} -> G
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    auto d = pipeline.emplace([] {}).name("D");
    auto e = pipeline.emplace([] {}).name("E");
    auto f = pipeline.emplace([] {}).name("F");
    auto g = pipeline.emplace([] {}).name("G");

    a.precede(b, c);
    d.succeed(b, c);
    d.precede(e, f);
    g.succeed(e, f);

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());

    const auto& order = exec.order();
    auto pos = [&](const std::string& name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };

    CHECK(pos("A") < pos("B"));  CHECK(pos("A") < pos("C"));
    CHECK(pos("B") < pos("D"));  CHECK(pos("C") < pos("D"));
    CHECK(pos("D") < pos("E"));  CHECK(pos("D") < pos("F"));
    CHECK(pos("E") < pos("G"));  CHECK(pos("F") < pos("G"));

    // Re-run to verify epoch reset through double-diamond
    exec.clear();
    auto r2 = pipeline.run(exec);
    REQUIRE(r2.has_value());
    CHECK(exec.order().front() == "A");
    CHECK(exec.order().back() == "G");
}

TEST_CASE("Pipeline: W-shape DAG with cross-mesh edges")
{
    //   A -> {B,C}, {B,C} -> D, {A,D} -> E
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    auto d = pipeline.emplace([] {}).name("D");
    auto e = pipeline.emplace([] {}).name("E");

    a.precede(b, c);
    d.succeed(b, c);
    e.succeed(a, d);  // E depends on BOTH A and D

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());

    const auto& order = exec.order();
    auto pos = [&](const std::string& name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };

    CHECK(pos("A") < pos("B"));
    CHECK(pos("A") < pos("C"));
    CHECK(pos("B") < pos("D"));
    CHECK(pos("C") < pos("D"));
    CHECK(pos("A") < pos("E"));
    CHECK(pos("D") < pos("E"));
}

TEST_CASE("Pipeline: hourglass (fan-out -> narrow -> fan-out)")
{
    //   root -> {L0..L4} -> mid -> {R0..R4}
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto root = pipeline.emplace([] {}).name("root");
    auto mid  = pipeline.emplace([] {}).name("mid");

    std::vector<Job> left, right;
    for (int i = 0; i < 5; ++i)
    {
        left.push_back(pipeline.emplace([] {}).name("L" + std::to_string(i)));
        left.back().succeed(root);
        mid.succeed(left.back());
    }
    for (int i = 0; i < 5; ++i)
    {
        right.push_back(pipeline.emplace([] {}).name("R" + std::to_string(i)));
        right.back().succeed(mid);
    }

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());

    const auto& order = exec.order();
    auto pos = [&](const std::string& name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };

    CHECK(order.front() == "root");
    auto midPos = pos("mid");
    for (int i = 0; i < 5; ++i)
    {
        CHECK(pos("L" + std::to_string(i)) < midPos);
        CHECK(pos("R" + std::to_string(i)) > midPos);
    }
    CHECK(pipeline.size() == 12U);
}

TEST_CASE("Pipeline: binary tree depth 5 (31 nodes)")
{
    // Level 0: 1 root, Level 1: 2, Level 2: 4, ... Level 4: 16
    // Total: 1+2+4+8+16 = 31 nodes
    Pipeline          pipeline;
    RecordingExecutor exec;

    std::vector<std::vector<Job>> levels(5);
    levels[0].push_back(pipeline.emplace([] {}).name("L0_0"));

    for (int depth = 1; depth < 5; ++depth)
    {
        for (std::size_t i = 0; i < levels[static_cast<std::size_t>(depth) - 1].size(); ++i)
        {
            auto left  = pipeline.emplace([] {}).name("L" + std::to_string(depth) + "_" + std::to_string(i * 2));
            auto right = pipeline.emplace([] {}).name("L" + std::to_string(depth) + "_" + std::to_string(i * 2 + 1));
            left.succeed(levels[static_cast<std::size_t>(depth) - 1][i]);
            right.succeed(levels[static_cast<std::size_t>(depth) - 1][i]);
            levels[static_cast<std::size_t>(depth)].push_back(left);
            levels[static_cast<std::size_t>(depth)].push_back(right);
        }
    }

    CHECK(pipeline.size() == 31U);

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(exec.order().size() == 31U);
    CHECK(exec.order().front() == "L0_0");

    // Re-run the tree
    exec.clear();
    auto r2 = pipeline.run(exec);
    REQUIRE(r2.has_value());
    CHECK(exec.order().size() == 31U);
}

TEST_CASE("Pipeline: large fan-out + fan-in stress (100 nodes)")
{
    constexpr int     cN = 100;
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto root = pipeline.emplace([] {}).name("root");
    auto sink = pipeline.emplace([] {}).name("sink");

    for (int i = 0; i < cN; ++i)
    {
        auto j = pipeline.emplace([] {}).name("w" + std::to_string(i));
        j.succeed(root);
        sink.succeed(j);
    }

    CHECK(pipeline.size() == static_cast<std::size_t>(cN + 2));

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(exec.order().front() == "root");
    CHECK(exec.order().back() == "sink");

    // Re-run
    exec.clear();
    auto r2 = pipeline.run(exec);
    REQUIRE(r2.has_value());
    CHECK(exec.order().front() == "root");
    CHECK(exec.order().back() == "sink");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Validation edge cases
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: cycle detection in longer cycle (A->B->C->A)")
{
    Pipeline pipeline;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    auto c = pipeline.emplace([] {}).name("C");
    a.precede(b);
    b.precede(c);
    c.precede(a);  // creates cycle

    auto result = pipeline.validate();
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCyclicDependency);
}

TEST_CASE("Pipeline: duplicate edges are tolerated")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    a.precede(b);
    a.precede(b);  // duplicate edge

    // Should still validate and run (duplicate deps just mean unmetDeps_ > 1
    // but all are satisfied by the same predecessor)
    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(exec.order().size() == 2U);
    CHECK(exec.order()[0] == "A");
    CHECK(exec.order()[1] == "B");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Boundary conditions
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Pipeline: jobs with empty names run correctly")
{
    Pipeline          pipeline;
    RecordingExecutor exec;
    int               counter = 0;

    auto a = pipeline.emplace([&] { ++counter; });  // no .name() call
    auto b = pipeline.emplace([&] { ++counter; });
    b.succeed(a);

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(counter == 2);
    CHECK(exec.order().size() == 2U);
}

TEST_CASE("Pipeline: job handles remain valid across re-runs")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    auto a = pipeline.emplace([] {}).name("A");
    auto b = pipeline.emplace([] {}).name("B");
    b.succeed(a);

    // Run 1
    (void)pipeline.run(exec);

    // Handles still work for queries after run
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(a.pipeline() == &pipeline);
    CHECK(pipeline.name(a) == "A");
    CHECK(pipeline.status(a) == JobStatus::kDone);

    // Run 2 -- same handles still valid
    (void)pipeline.run(exec);
    CHECK(a.valid());
    CHECK(pipeline.name(a) == "A");
    CHECK(pipeline.status(a) == JobStatus::kDone);
}

TEST_CASE("Pipeline: observer progress is monotonic and resets on re-run")
{
    Pipeline          pipeline;
    RecordingExecutor exec;

    for (int i = 0; i < 5; ++i)
        pipeline.emplace([] {}).name("j" + std::to_string(i));

    struct ProgressObserver final : IObserver
    {
        std::vector<float> progressValues;
        float lastProgress = -1.0f;
        bool monotonic = true;
        void onJobStart(RunId, JobId, std::string_view) override {}
        void onJobFinish(RunId, JobId, std::string_view, JobStatus, float progress) override
        {
            progressValues.push_back(progress);
            if (progress < lastProgress) monotonic = false;
            lastProgress = progress;
        }
        void reset() { progressValues.clear(); lastProgress = -1.0f; monotonic = true; }
    } obs;

    // Run 1
    (void)pipeline.run(exec, &obs);
    CHECK(obs.monotonic);
    REQUIRE(!obs.progressValues.empty());
    CHECK(obs.progressValues.back() == doctest::Approx(1.0f));

    // Run 2 -- progress should start from scratch
    obs.reset();
    (void)pipeline.run(exec, &obs);
    CHECK(obs.monotonic);
    CHECK(obs.progressValues.back() == doctest::Approx(1.0f));
}

TEST_CASE("Tick loop: stop token returns at a complete tick-pass boundary")
{
    TickLoop ticks;
    std::stop_source stop;
    int stopTickCount = 0;
    int laterTickCount = 0;

    ticks.add({   // request-stop
        .interval = 0ms,
        .fn = [&] {
            ++stopTickCount;
            stop.request_stop();
        }
    });
    ticks.add({   // finish-current-pass
        .interval = 0ms,
        .fn = [&] { ++laterTickCount; }
    });

    ticks.run(stop.get_token());

    CHECK(stopTickCount == 1);
    CHECK(laterTickCount == 1);
}

TEST_CASE("Tick loop: an already-requested stop dispatches no ticks")
{
    TickLoop ticks;
    std::stop_source stop;
    int tickCount = 0;
    stop.request_stop();

    ticks.add({   // must-not-run
        .interval = 0ms,
        .fn = [&] { ++tickCount; }
    });

    ticks.run(stop.get_token());

    CHECK(tickCount == 0);
}

TEST_CASE("Tick loop: external stop waits for the active callback to finish")
{
    TickLoop ticks;
    std::latch tickStarted{1};
    std::latch finishTick{1};
    int tickCount = 0;

    ticks.add({   // blocked-tick
        .interval = 0ms,
        .fn = [&] {
            tickStarted.count_down();
            finishTick.wait();
            ++tickCount;
        }
    });

    std::jthread loop([&](std::stop_token stop) {
        ticks.run(stop);
    });

    tickStarted.wait();
    loop.request_stop();
    finishTick.count_down();
    loop.join();

    CHECK(tickCount == 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Library inline executors: run_inline() and SequentialExecutor
// ═══════════════════════════════════════════════════════════════════════════════

// These used to call each job from inside its predecessor's completion, so
// stack depth grew with the chain: 2,000 links overflowed a 1 MB stack.

TEST_CASE("Inline executors: a 20000-job chain runs without recursing per link")
{
    constexpr int cJobs = 20000;
    Pipeline pipeline;
    pipeline.reserve(cJobs);
    int ran = 0;
    Job previous;
    for (int i = 0; i < cJobs; ++i)
    {
        auto job = pipeline.emplace([&] { ++ran; });
        if (previous.valid()) job.succeed(previous);
        previous = job;
    }

    CHECK(pipeline.run_inline().has_value());
    CHECK(ran == cJobs);

    SequentialExecutor sequential;
    CHECK(pipeline.run(sequential).has_value());
    CHECK(ran == 2 * cJobs);
}

TEST_CASE("Inline executors: jobs run in the order they become ready")
{
    //   a -> b -> e
    //   a -> c
    //   b, c -> d
    // b's successor e becomes ready before c has run, but c was dispatched first.
    Pipeline pipeline;
    std::vector<char> order;
    auto a = pipeline.emplace([&] { order.push_back('a'); });
    auto b = pipeline.emplace([&] { order.push_back('b'); });
    auto c = pipeline.emplace([&] { order.push_back('c'); });
    auto d = pipeline.emplace([&] { order.push_back('d'); });
    auto e = pipeline.emplace([&] { order.push_back('e'); });
    a.precede(b, c);
    d.succeed(b, c);
    e.succeed(b);

    const std::vector<char> expected{'a', 'b', 'c', 'e', 'd'};
    CHECK(pipeline.run_inline().has_value());
    CHECK(order == expected);

    order.clear();
    SequentialExecutor sequential;
    CHECK(pipeline.run(sequential).has_value());
    CHECK(order == expected);
}

TEST_CASE("Inline executors: a job can run a nested pipeline on the same executor")
{
    SequentialExecutor sequential;
    Pipeline outer;
    std::vector<int> order;
    auto first = outer.emplace([&]() -> std::expected<void, PipelineError> {
        Pipeline inner;
        auto one = inner.emplace([&] { order.push_back(1); });
        inner.emplace([&] { order.push_back(2); }).succeed(one);
        auto result = inner.run(sequential);   // must finish before this job returns
        order.push_back(3);
        return result;
    });
    outer.emplace([&] { order.push_back(4); }).succeed(first);

    CHECK(outer.run(sequential).has_value());
    CHECK(order == std::vector<int>{1, 2, 3, 4});
}
