// tests/test_dsl.cpp
//
// Tests for the Sub0Pipeline DSL extension: operators, _job UDL, JobSpec,
// JobSpecGroup, and inline pipe syntax.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <sub0pipeline/dsl.hpp>
#include "doctest.h"
#include "test_helpers.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace sub0pipeline;
using namespace sub0pipeline::dsl;
using namespace std::chrono_literals;

// ── Test helpers ─────────────────────────────────────────────────────────────

class RecordingExecutor final : public IExecutor
{
public:
    void dispatch(
        std::string_view name, std::function<void()> fn,
        std::function<void()> onComplete,
        int, uint8_t, uint32_t) override
    {
        order_.emplace_back(name);
        fn();
        if (onComplete) onComplete();
    }

    void wait_all() override {}
    [[nodiscard]] int concurrency() const noexcept override { return 1; }

    [[nodiscard]] const std::vector<std::string>& order() const { return order_; }

private:
    std::vector<std::string> order_;
};

// ═══════════════════════════════════════════════════════════════════════════════
// _job UDL + JobSpec
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("_job UDL creates JobNameProxy")
{
    auto proxy = "hello"_job;
    CHECK(proxy.name == "hello");
}

TEST_CASE("_job(fn) creates JobSpec that builds a named job")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto j = pipe.emplace("foo"_job([] {}));
    CHECK(j.valid());
    CHECK(pipe.name(j) == "foo");

    (void)pipe.run(exec);
    CHECK(pipe.status(j) == JobStatus::kDone);
}

TEST_CASE("_job(fn).timeout() chains builder methods")
{
    Pipeline pipe;
    auto j = pipe.emplace("timed"_job([] {}).timeout(500ms));
    CHECK(j.valid());
    CHECK(pipe.name(j) == "timed");
}

TEST_CASE("_job(fn).optional() marks job optional")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto j = pipe.emplace("opt"_job([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).optional());

    auto result = pipe.run(exec);
    CHECK(result.has_value());
    CHECK(pipe.status(j) == JobStatus::kFailed);
}

TEST_CASE("job(fn) creates unnamed JobSpec")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto j = pipe.emplace(job([] {}));
    CHECK(j.valid());

    (void)pipe.run(exec);
    CHECK(pipe.status(j) == JobStatus::kDone);
}

TEST_CASE("Multi-emplace with _job returns tuple for structured bindings")
{
    Pipeline pipe;

    auto [a, b, c] = pipe.emplace(
        "A"_job([] {}),
        "B"_job([] {}),
        "C"_job([] {})
    );

    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(c.valid());
    CHECK(pipe.name(a) == "A");
    CHECK(pipe.name(b) == "B");
    CHECK(pipe.name(c) == "C");
    CHECK(pipe.size() == 3U);
}

// ═══════════════════════════════════════════════════════════════════════════════
// operator>> (Job >> Job)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("a >> b wires A before B, returns B")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto a = pipe.emplace([] {}).name("A");
    auto b = pipe.emplace([] {}).name("B");

    auto result = a >> b;
    CHECK(result == b);

    (void)pipe.run(exec);
    const auto& order = exec.order();
    REQUIRE(order.size() == 2U);
    CHECK(order[0] == "A");
    CHECK(order[1] == "B");
}

TEST_CASE("a >> b >> c creates linear chain")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto a = pipe.emplace([] {}).name("A");
    auto b = pipe.emplace([] {}).name("B");
    auto c = pipe.emplace([] {}).name("C");

    a >> b >> c;

    (void)pipe.run(exec);
    const auto& order = exec.order();
    REQUIRE(order.size() == 3U);
    CHECK(order[0] == "A");
    CHECK(order[1] == "B");
    CHECK(order[2] == "C");
}

// ═══════════════════════════════════════════════════════════════════════════════
// operator+ (parallel grouping)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("a + b creates JobGroup with no dependencies")
{
    Pipeline pipe;
    auto a = pipe.emplace([] {}).name("A");
    auto b = pipe.emplace([] {}).name("B");

    auto group = a + b;
    CHECK(group.jobs().size() == 2U);
}

TEST_CASE("a + b >> c: fan-in (both A and B precede C)")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto a = pipe.emplace([] {}).name("A");
    auto b = pipe.emplace([] {}).name("B");
    auto c = pipe.emplace([] {}).name("C");

    a + b >> c;

    (void)pipe.run(exec);
    const auto& order = exec.order();
    auto cPos = std::find(order.begin(), order.end(), "C") - order.begin();
    auto aPos = std::find(order.begin(), order.end(), "A") - order.begin();
    auto bPos = std::find(order.begin(), order.end(), "B") - order.begin();
    CHECK(cPos > aPos);
    CHECK(cPos > bPos);
}

TEST_CASE("a >> b + c: fan-out (A precedes both B and C)")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto a = pipe.emplace([] {}).name("A");
    auto b = pipe.emplace([] {}).name("B");
    auto c = pipe.emplace([] {}).name("C");

    a >> b + c;

    (void)pipe.run(exec);
    const auto& order = exec.order();
    auto aPos = std::find(order.begin(), order.end(), "A") - order.begin();
    auto bPos = std::find(order.begin(), order.end(), "B") - order.begin();
    auto cPos = std::find(order.begin(), order.end(), "C") - order.begin();
    CHECK(bPos > aPos);
    CHECK(cPos > aPos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Full diamond
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("nvs >> display + network >> app: full diamond")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto nvs     = pipe.emplace([] {}).name("nvs");
    auto display = pipe.emplace([] {}).name("display");
    auto network = pipe.emplace([] {}).name("network");
    auto app     = pipe.emplace([] {}).name("app");

    nvs >> display + network >> app;

    (void)pipe.run(exec);
    const auto& order = exec.order();
    REQUIRE(order.size() == 4U);
    CHECK(order.front() == "nvs");
    CHECK(order.back() == "app");
}

TEST_CASE("root >> a + b + c >> sink: triple fan-out/in")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto root = pipe.emplace([] {}).name("root");
    auto a    = pipe.emplace([] {}).name("A");
    auto b    = pipe.emplace([] {}).name("B");
    auto c    = pipe.emplace([] {}).name("C");
    auto sink = pipe.emplace([] {}).name("sink");

    root >> a + b + c >> sink;

    (void)pipe.run(exec);
    const auto& order = exec.order();
    CHECK(order.front() == "root");
    CHECK(order.back() == "sink");
}

TEST_CASE("(a + b) >> (c + d): JobGroup >> JobGroup all-pairs")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto a = pipe.emplace([] {}).name("A");
    auto b = pipe.emplace([] {}).name("B");
    auto c = pipe.emplace([] {}).name("C");
    auto d = pipe.emplace([] {}).name("D");

    (a + b) >> (c + d);

    (void)pipe.run(exec);
    const auto& order = exec.order();
    auto aPos = std::find(order.begin(), order.end(), "A") - order.begin();
    auto bPos = std::find(order.begin(), order.end(), "B") - order.begin();
    auto cPos = std::find(order.begin(), order.end(), "C") - order.begin();
    auto dPos = std::find(order.begin(), order.end(), "D") - order.begin();
    CHECK(cPos > aPos);
    CHECK(cPos > bPos);
    CHECK(dPos > aPos);
    CHECK(dPos > bPos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Dynamic grouping
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Dynamic loop: group = group + workers[i]")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto root = pipe.emplace([] {}).name("root");
    auto sink = pipe.emplace([] {}).name("sink");

    std::vector<Job> workers;
    for (int i = 0; i < 4; ++i)
        workers.push_back(pipe.emplace([] {}).name("w" + std::to_string(i)));

    auto group = workers[0] + workers[1];
    for (std::size_t i = 2; i < workers.size(); ++i)
        group = group + workers[i];

    root >> group >> sink;

    (void)pipe.run(exec);
    const auto& order = exec.order();
    CHECK(order.front() == "root");
    CHECK(order.back() == "sink");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Inline pipe syntax (Pipeline/Job >> JobSpec)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("pipe >> _job(fn) >> _job(fn): inline linear chain")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    pipe >> "A"_job([] {}) >> "B"_job([] {}) >> "C"_job([] {});

    (void)pipe.run(exec);
    const auto& order = exec.order();
    REQUIRE(order.size() == 3U);
    CHECK(order[0] == "A");
    CHECK(order[1] == "B");
    CHECK(order[2] == "C");
}

TEST_CASE("pipe >> job(fn) >> job(fn): unnamed inline pipe")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    pipe >> job([] {}) >> job([] {}) >> job([] {});

    (void)pipe.run(exec);
    CHECK(exec.order().size() == 3U);
}

TEST_CASE("Inline pipe with fan-out/in via JobSpecGroup")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    pipe >> "A"_job([] {})
         >> "B"_job([] {}) + "C"_job([] {})
         >> "D"_job([] {});

    (void)pipe.run(exec);
    const auto& order = exec.order();
    REQUIRE(order.size() == 4U);
    CHECK(order.front() == "A");
    CHECK(order.back() == "D");
}

TEST_CASE("JobSpec + JobSpec creates JobSpecGroup without emplacement")
{
    // Just verify it compiles and constructs — no pipeline needed.
    auto group = "A"_job([] {}) + "B"_job([] {});
    (void)group;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Mixed: emplaced root/sink + inline middle
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Emplaced root >> inline specs >> emplaced sink")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto root = pipe.emplace("root"_job([] {}));
    auto sink = pipe.emplace("sink"_job([] {}));

    root >> "mid_a"_job([] {}) + "mid_b"_job([] {}) >> sink;

    (void)pipe.run(exec);
    const auto& order = exec.order();
    CHECK(order.front() == "root");
    CHECK(order.back() == "sink");
    CHECK(order.size() == 4U);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Pipeline >> JobSpecGroup (parallel emplace)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("pipe >> a+b+c: returns JobTuple with correct bindings, no deps between them")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto [a, b, c] = pipe >> "A"_job([] {}) + "B"_job([] {}) + "C"_job([] {});

    // All bindings are valid and correctly named
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(c.valid());
    CHECK(pipe.name(a) == "A");
    CHECK(pipe.name(b) == "B");
    CHECK(pipe.name(c) == "C");

    // Each binding is a distinct job
    CHECK_FALSE(a == b);
    CHECK_FALSE(b == c);
    CHECK_FALSE(a == c);

    // All belong to the same pipeline
    CHECK(a.pipeline() == &pipe);
    CHECK(b.pipeline() == &pipe);
    CHECK(c.pipeline() == &pipe);

    CHECK(pipe.size() == 3U);
    CHECK(pipe.validate().has_value());

    auto result = pipe.run(exec);
    REQUIRE(result.has_value());
    CHECK(exec.order().size() == 3U);

    // All jobs completed
    CHECK(pipe.status(a) == JobStatus::kDone);
    CHECK(pipe.status(b) == JobStatus::kDone);
    CHECK(pipe.status(c) == JobStatus::kDone);
}

TEST_CASE("auto [a,b,c] = pipe >> specs >> sink: all wired to sink")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto sink = pipe.emplace("sink"_job([] {}));
    auto [a, b, c] = pipe >> "A"_job([] {}) + "B"_job([] {}) + "C"_job([] {}) >> sink;

    // Verify all bindings
    CHECK(pipe.name(a) == "A");
    CHECK(pipe.name(b) == "B");
    CHECK(pipe.name(c) == "C");
    CHECK_FALSE(a == b);
    CHECK_FALSE(b == c);

    CHECK(pipe.size() == 4U);
    CHECK(pipe.validate().has_value());

    auto result = pipe.run(exec);
    REQUIRE(result.has_value());

    const auto& order = exec.order();
    REQUIRE(order.size() == 4U);

    // sink must run after ALL of A, B, C
    auto sinkPos = std::find(order.begin(), order.end(), "sink") - order.begin();
    auto aPos    = std::find(order.begin(), order.end(), "A")    - order.begin();
    auto bPos    = std::find(order.begin(), order.end(), "B")    - order.begin();
    auto cPos    = std::find(order.begin(), order.end(), "C")    - order.begin();
    CHECK(sinkPos > aPos);
    CHECK(sinkPos > bPos);
    CHECK(sinkPos > cPos);
    CHECK(order.back() == "sink");

    // All kDone
    CHECK(pipe.status(a)    == JobStatus::kDone);
    CHECK(pipe.status(b)    == JobStatus::kDone);
    CHECK(pipe.status(c)    == JobStatus::kDone);
    CHECK(pipe.status(sink) == JobStatus::kDone);
}

TEST_CASE("pipe >> a+b+c >> d+e+f >> sink: two-layer chain, full verification")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto sink = pipe.emplace("sink"_job([] {}));

    auto [l1, l2] = pipe >> "A"_job([] {}) + "B"_job([] {}) + "C"_job([] {})
                         >> "D"_job([] {}) + "E"_job([] {}) + "F"_job([] {})
                         >> sink;

    // Destructure and verify ALL bindings
    auto [a, b, c] = l1;
    auto [d, e, f] = l2;
    CHECK(pipe.name(a) == "A");
    CHECK(pipe.name(b) == "B");
    CHECK(pipe.name(c) == "C");
    CHECK(pipe.name(d) == "D");
    CHECK(pipe.name(e) == "E");
    CHECK(pipe.name(f) == "F");

    // All distinct
    CHECK_FALSE(a == b); CHECK_FALSE(a == c); CHECK_FALSE(a == d);
    CHECK_FALSE(b == c); CHECK_FALSE(b == e);
    CHECK_FALSE(d == e); CHECK_FALSE(d == f); CHECK_FALSE(e == f);

    CHECK(pipe.size() == 7U);
    CHECK(pipe.validate().has_value());

    auto result = pipe.run(exec);
    REQUIRE(result.has_value());

    const auto& order = exec.order();
    REQUIRE(order.size() == 7U);

    auto pos = [&](const std::string& name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };

    // Layer 1 (A,B,C) must ALL come before EVERY job in layer 2 (D,E,F)
    // This verifies the cross-product dependency wiring
    CHECK(pos("A") < pos("D"));  CHECK(pos("A") < pos("E"));  CHECK(pos("A") < pos("F"));
    CHECK(pos("B") < pos("D"));  CHECK(pos("B") < pos("E"));  CHECK(pos("B") < pos("F"));
    CHECK(pos("C") < pos("D"));  CHECK(pos("C") < pos("E"));  CHECK(pos("C") < pos("F"));

    // Layer 2 must ALL come before sink
    CHECK(pos("D") < pos("sink"));
    CHECK(pos("E") < pos("sink"));
    CHECK(pos("F") < pos("sink"));

    // Sink is last
    CHECK(order.back() == "sink");

    // All kDone
    CHECK(pipe.status(a)    == JobStatus::kDone);
    CHECK(pipe.status(b)    == JobStatus::kDone);
    CHECK(pipe.status(c)    == JobStatus::kDone);
    CHECK(pipe.status(d)    == JobStatus::kDone);
    CHECK(pipe.status(e)    == JobStatus::kDone);
    CHECK(pipe.status(f)    == JobStatus::kDone);
    CHECK(pipe.status(sink) == JobStatus::kDone);
}

TEST_CASE("pipe >> a+b+c >> d+e+f: two layers without sink, full verification")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto [l1, l2] = pipe >> "A"_job([] {}) + "B"_job([] {}) + "C"_job([] {})
                         >> "D"_job([] {}) + "E"_job([] {}) + "F"_job([] {});

    // Verify ALL bindings
    auto [a, b, c] = l1;
    auto [d, e, f] = l2;
    CHECK(pipe.name(a) == "A");
    CHECK(pipe.name(b) == "B");
    CHECK(pipe.name(c) == "C");
    CHECK(pipe.name(d) == "D");
    CHECK(pipe.name(e) == "E");
    CHECK(pipe.name(f) == "F");

    CHECK(pipe.size() == 6U);
    CHECK(pipe.validate().has_value());

    auto result = pipe.run(exec);
    REQUIRE(result.has_value());

    const auto& order = exec.order();
    REQUIRE(order.size() == 6U);

    auto pos = [&](const std::string& name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };

    // Full cross-product: every L1 job before every L2 job
    CHECK(pos("A") < pos("D"));  CHECK(pos("A") < pos("E"));  CHECK(pos("A") < pos("F"));
    CHECK(pos("B") < pos("D"));  CHECK(pos("B") < pos("E"));  CHECK(pos("B") < pos("F"));
    CHECK(pos("C") < pos("D"));  CHECK(pos("C") < pos("E"));  CHECK(pos("C") < pos("F"));

    // All kDone
    for (auto j : {a, b, c, d, e, f})
        CHECK(pipe.status(j) == JobStatus::kDone);
}

TEST_CASE("Three-layer chain: pipe >> l1 >> l2 >> l3 >> sink, full verification")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto sink = pipe.emplace("sink"_job([] {}));

    auto [l1, l2, l3] = pipe >> "A"_job([] {}) + "B"_job([] {})
                              >> "C"_job([] {}) + "D"_job([] {})
                              >> "E"_job([] {}) + "F"_job([] {})
                              >> sink;

    // Verify ALL bindings across all layers
    auto [a, b] = l1;
    auto [c, d] = l2;
    auto [e, f] = l3;
    CHECK(pipe.name(a) == "A");
    CHECK(pipe.name(b) == "B");
    CHECK(pipe.name(c) == "C");
    CHECK(pipe.name(d) == "D");
    CHECK(pipe.name(e) == "E");
    CHECK(pipe.name(f) == "F");

    // All distinct
    CHECK_FALSE(a == b); CHECK_FALSE(a == c); CHECK_FALSE(a == d);
    CHECK_FALSE(a == e); CHECK_FALSE(a == f);
    CHECK_FALSE(c == d); CHECK_FALSE(e == f);

    CHECK(pipe.size() == 7U);
    CHECK(pipe.validate().has_value());

    auto result = pipe.run(exec);
    REQUIRE(result.has_value());

    const auto& order = exec.order();
    REQUIRE(order.size() == 7U);

    auto pos = [&](const std::string& name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };

    // L1 (A,B) before ALL of L2 (C,D) — cross-product
    CHECK(pos("A") < pos("C"));  CHECK(pos("A") < pos("D"));
    CHECK(pos("B") < pos("C"));  CHECK(pos("B") < pos("D"));

    // L2 (C,D) before ALL of L3 (E,F) — cross-product
    CHECK(pos("C") < pos("E"));  CHECK(pos("C") < pos("F"));
    CHECK(pos("D") < pos("E"));  CHECK(pos("D") < pos("F"));

    // L3 (E,F) before sink
    CHECK(pos("E") < pos("sink"));
    CHECK(pos("F") < pos("sink"));

    // Sink is last
    CHECK(order.back() == "sink");

    // All kDone
    for (auto j : {a, b, c, d, e, f, sink})
        CHECK(pipe.status(j) == JobStatus::kDone);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Interop: DSL + builder API
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("DSL operators compose with .name() / .timeout() / .optional()")
{
    Pipeline          pipe;
    RecordingExecutor exec;

    auto sensor = pipe.emplace([] {}).name("sensor").timeout(100ms);
    auto proc   = pipe.emplace("proc"_job([] {}).optional());
    auto log    = pipe.emplace("log"_job([] {}));

    sensor >> proc >> log;

    (void)pipe.run(exec);
    const auto& order = exec.order();
    REQUIRE(order.size() == 3U);
    CHECK(order[0] == "sensor");
    CHECK(order[1] == "proc");
    CHECK(order[2] == "log");
}
