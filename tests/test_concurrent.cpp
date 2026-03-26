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
        std::lock_guard lk{mtx_};
        threads_.emplace_back([fn = std::move(fn), oc = std::move(on_complete)]
        {
            fn();
            if (oc) oc();
        });
    }

    void wait_all() override
    {
        // Drain loop: successor jobs may be dispatched during execution, so keep
        // joining until all threads are exhausted.
        while (true) {
            std::vector<std::thread> batch;
            {
                std::lock_guard lk{mtx_};
                if (threads_.empty()) break;
                batch = std::move(threads_);
            }
            for (auto& t : batch) {
                if (t.joinable()) t.join();
            }
        }
    }

    [[nodiscard]] int concurrency() const noexcept override { return 2; }

private:
    std::mutex               mtx_;
    std::vector<std::thread> threads_;
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
    constexpr int      N = 50;

    auto root = pipeline.emplace([&] { counter.fetch_add(1, std::memory_order_relaxed); }).name("root");
    for (int i = 0; i < N; ++i) {
        pipeline.emplace([&] { counter.fetch_add(1, std::memory_order_relaxed); })
            .name("task_" + std::to_string(i))
            .succeed(root);
    }

    auto result = pipeline.run(exec);
    REQUIRE(result.has_value());
    CHECK(counter.load() == N + 1);
}

TEST_CASE("Concurrent: DesktopExecutor smoke test — sequential pipeline succeeds")
{
    // Requires Sub0Pipeline_Desktop to be linked.
    // Declared in desktop_executor.cpp.
    std::unique_ptr<IExecutor> exec = make_desktop_executor();
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
