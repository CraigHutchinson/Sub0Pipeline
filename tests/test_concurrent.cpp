// tests/test_concurrent.cpp
//
// Thread-safety tests using real std::thread parallelism.
// Validates that the Pipeline DAG engine is safe under concurrent dispatch.

#include <sub0pipeline/sub0pipeline.hpp>
#include "doctest.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace sub0pipeline { std::unique_ptr<IExecutor> makeDesktopExecutor(); }

using namespace sub0pipeline;

// ── Thread-pool executor (one thread per dispatched job) ──────────────────────

class ThreadPoolExecutor final : public IExecutor
{
public:
    void dispatch(
        std::string_view              /*name*/,
        std::function<void()>         fn,
        std::function<void()>         on_complete,
        int                           /*core*/,
        uint8_t                       /*priority*/,
        uint32_t                      /*stack_bytes*/) override
    {
        inFlight_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lk{mtx_};
        threads_.emplace_back([this, fn = std::move(fn), oc = std::move(on_complete)]
        {
            fn();
            if (oc) oc();
            inFlight_.fetch_sub(1, std::memory_order_relaxed);
        });
    }

    void wait_all() override
    {
        // Drain loop: successor jobs may be dispatched during execution, so keep
        // joining until all threads are exhausted and inFlight_ reaches zero.
        while (true) {
            std::vector<std::thread> batch;
            {
                std::lock_guard lk{mtx_};
                if (threads_.empty() && inFlight_.load(std::memory_order_relaxed) == 0) break;
                batch = std::move(threads_);
            }
            for (auto& t : batch) {
                if (t.joinable()) t.join();
            }
        }
    }

    [[nodiscard]] int concurrency() const noexcept override { return 2; }

private:
    std::mutex                mtx_;
    std::vector<std::thread>  threads_;
    std::atomic<uint32_t>     inFlight_{0};
};

// ═══════════════════════════════════════════════════════════════════════════════
// Concurrent execution tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Concurrent: diamond DAG with real threads — all jobs run exactly once")
{
    Pipeline              pipeline;
    std::atomic<int>      counter{0};
    ThreadPoolExecutor    exec;

    auto a = pipeline.emplace([&] { counter.fetch_add(1,    std::memory_order_relaxed); }).name("A");
    auto b = pipeline.emplace([&] { counter.fetch_add(10,   std::memory_order_relaxed); }).name("B");
    auto c = pipeline.emplace([&] { counter.fetch_add(100,  std::memory_order_relaxed); }).name("C");
    auto d = pipeline.emplace([&] { counter.fetch_add(1000, std::memory_order_relaxed); }).name("D");

    a.precede(b, c);
    d.succeed(b, c);

    auto result = pipeline.run(exec);

    REQUIRE(result.has_value());
    CHECK(counter.load() == 1111);
}

TEST_CASE("Concurrent: wide fan-out stress N=50 with real threads")
{
    Pipeline           pipeline;
    std::atomic<int>   counter{0};
    ThreadPoolExecutor exec;
    constexpr int      cN = 50;

    auto root = pipeline.emplace([&] { counter.fetch_add(1, std::memory_order_relaxed); }).name("root");
    for (int i = 0; i < cN; ++i) {
        pipeline.emplace([&] { counter.fetch_add(1, std::memory_order_relaxed); })
            .name("task_" + std::to_string(i))
            .succeed(root);
    }

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(counter.load() == cN + 1);
}

TEST_CASE("Concurrent: DesktopExecutor smoke test — sequential pipeline succeeds")
{
    // Requires Sub0Pipeline_Desktop to be linked.
    // Declared in desktop_executor.cpp.
    std::unique_ptr<IExecutor> exec = makeDesktopExecutor();
    REQUIRE(exec != nullptr);

    Pipeline pipeline;
    int counter = 0;

    auto a = pipeline.emplace([&] { ++counter; }).name("A");
    auto b = pipeline.emplace([&] { ++counter; }).name("B");
    b.succeed(a);

    auto result = pipeline.run(*exec);
    REQUIRE(result.has_value());
    CHECK(counter == 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Sub-DAG execution (ScopedExecutor)
// ═══════════════════════════════════════════════════════════════════════════════

namespace sub0pipeline { std::unique_ptr<IExecutor> makeSequentialExecutor(); }

TEST_CASE("SubDAG: sequential -- job creates and runs an inner pipeline inline")
{
    auto exec = makeSequentialExecutor();
    Pipeline outer;

    int innerRuns = 0;
    outer.emplace([&]() -> std::expected<void, PipelineError>
    {
        Pipeline inner;
        inner.emplace([&]{ ++innerRuns; }).name("inner_a");
        inner.emplace([&]{ ++innerRuns; }).name("inner_b");
        return inner.run(*exec);
    }).name("dynamic_step");

    auto result = outer.run(*exec);
    CHECK(result.has_value());
    CHECK(innerRuns == 2);
}

TEST_CASE("SubDAG: ScopedExecutor -- desktop job creates dynamic inner pipeline without deadlock")
{
    auto exec = makeDesktopExecutor();
    Pipeline outer;

    std::atomic<int> innerRuns{0};
    outer.emplace([&]() -> std::expected<void, PipelineError>
    {
        // ScopedExecutor shares the thread pool but scopes wait_all()
        // to only the inner jobs -- avoids the self-wait deadlock.
        ScopedExecutor scoped{*exec};
        Pipeline inner;
        inner.emplace([&]{ innerRuns.fetch_add(1, std::memory_order_relaxed); }).name("fetch_0");
        inner.emplace([&]{ innerRuns.fetch_add(1, std::memory_order_relaxed); }).name("fetch_1");
        inner.emplace([&]{ innerRuns.fetch_add(1, std::memory_order_relaxed); }).name("fetch_2");
        return inner.run(scoped);
    }).name("dynamic_step");

    auto result = outer.run(*exec);
    CHECK(result.has_value());
    CHECK(innerRuns.load() == 3);
}

TEST_CASE("SubDAG: ScopedExecutor -- dynamic job count determined at runtime")
{
    auto exec = makeDesktopExecutor();
    Pipeline outer;

    const int dynamicCount = 7;  // determined "at runtime"
    std::atomic<int> innerRuns{0};

    outer.emplace([&]() -> std::expected<void, PipelineError>
    {
        ScopedExecutor scoped{*exec};
        Pipeline inner;
        for (int i = 0; i < dynamicCount; ++i)
            inner.emplace([&]{ innerRuns.fetch_add(1, std::memory_order_relaxed); });
        return inner.run(scoped);
    }).name("dynamic_fan_out");

    auto result = outer.run(*exec);
    CHECK(result.has_value());
    CHECK(innerRuns.load() == dynamicCount);
}

TEST_CASE("SubDAG: ScopedExecutor -- inner DAG failure propagates to outer job")
{
    auto exec = makeDesktopExecutor();
    Pipeline outer;

    outer.emplace([&]() -> std::expected<void, PipelineError>
    {
        ScopedExecutor scoped{*exec};
        Pipeline inner;
        inner.emplace([]() -> std::expected<void, PipelineError> {
            return std::unexpected(PipelineError::kJobFailed);
        }).name("failing_inner");
        return inner.run(scoped);
    }).name("outer_job");

    auto result = outer.run(*exec);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
}
