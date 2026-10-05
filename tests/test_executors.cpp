// tests/test_executors.cpp
//
// The bundled executors as concrete types: constructed in place (no factory,
// no heap), configured through their own constructors, and used through the
// IExecutor interface by Pipeline.

#include <sub0pipeline/sub0pipeline.hpp>
#include "doctest.h"

#include <algorithm>
#include <atomic>
#include <latch>
#include <thread>
#include <type_traits>

using namespace sub0pipeline;

namespace {

// 1 root -> `leaves` leaves; returns how many jobs ran.
template<typename Executor>
int runFanOut(Executor& executor, int leaves)
{
    Pipeline pipeline;
    std::atomic<int> ran{0};
    auto root = pipeline.emplace([&] { ++ran; });
    for (int i = 0; i < leaves; ++i)
        pipeline.emplace([&] { ++ran; }).succeed(root);
    REQUIRE(pipeline.run(executor).has_value());
    return ran.load();
}

} // namespace

// Executors own threads or OS handles, so none of them may be copied or moved.
static_assert(!std::is_copy_constructible_v<DesktopExecutor>);
static_assert(!std::is_move_constructible_v<DesktopExecutor>);
static_assert(!std::is_copy_constructible_v<PriorityExecutor>);
static_assert(!std::is_move_constructible_v<PriorityExecutor>);
static_assert(std::is_default_constructible_v<SequentialExecutor>);
static_assert(std::is_default_constructible_v<PriorityExecutor>);

TEST_CASE("Executors: SequentialExecutor is usable as a plain local object")
{
    SequentialExecutor executor;
    CHECK(executor.concurrency() == 1);
    CHECK(executor.runs_inline());
    CHECK(runFanOut(executor, 8) == 9);
}

TEST_CASE("Executors: DesktopExecutor is usable as a local object and as a member")
{
    {
        DesktopExecutor executor;
        CHECK(runFanOut(executor, 8) == 9);
        CHECK(runFanOut(executor, 3) == 4);   // reusable after wait_all()
    }

    struct Owner {
        DesktopExecutor executor;
        Pipeline pipeline;
    } owner;
    std::atomic<bool> ran{false};
    (void)owner.pipeline.emplace([&] { ran = true; });
    CHECK(owner.pipeline.run(owner.executor).has_value());
    CHECK(ran.load());
}

TEST_CASE("Executors: PriorityExecutor defaults to one worker per hardware thread")
{
    PriorityExecutor executor;
    const int expected = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
    CHECK(executor.concurrency() == expected);
    CHECK(runFanOut(executor, 8) == 9);
}

TEST_CASE("Executors: PriorityExecutor::Options sets the worker count and per-thread hook")
{
    constexpr int cWorkers = 3;
    std::atomic<int> started{0};
    std::latch allStarted{cWorkers};

    PriorityExecutor executor{{
        .threadCount   = cWorkers,
        .onThreadStart = [&] { ++started; allStarted.count_down(); },
    }};
    CHECK(executor.concurrency() == cWorkers);

    allStarted.wait();
    CHECK(started.load() == cWorkers);
    CHECK(runFanOut(executor, 16) == 17);
    CHECK(started.load() == cWorkers);   // the hook runs once per worker, not per job
}

TEST_CASE("Executors: ScopedExecutor wraps a locally constructed pool")
{
    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline outer;
    std::atomic<int> inner{0};
    (void)outer.emplace([&]() -> std::expected<void, PipelineError> {
        ScopedExecutor scoped{pool};
        Pipeline nested;
        for (int i = 0; i < 4; ++i) (void)nested.emplace([&] { ++inner; });
        return nested.run(scoped);
    });
    CHECK(outer.run(pool).has_value());
    CHECK(inner.load() == 4);
}

TEST_CASE("Executors: the factories still return a working executor behind IExecutor")
{
    auto sequential = makeSequentialExecutor();
    auto desktop    = makeDesktopExecutor();
    auto pool       = makePriorityExecutor(2);
    CHECK(runFanOut(*sequential, 4) == 5);
    CHECK(runFanOut(*desktop, 4) == 5);
    CHECK(runFanOut(*pool, 4) == 5);
    CHECK(pool->concurrency() == 2);
}
