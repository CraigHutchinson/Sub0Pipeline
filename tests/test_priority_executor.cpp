// tests/test_priority_executor.cpp
//
// PriorityExecutor: real thread-pool behaviour tests (priority ordering,
// per-thread onThreadStart hook). Uses the real makePriorityExecutor()
// factory, not a fake IExecutor -- these properties only exist in the real
// thread-pool implementation.

#include <sub0pipeline/sub0pipeline.hpp>
#include "doctest.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace sub0pipeline;
using namespace std::chrono_literals;

TEST_CASE("PriorityExecutor: dispatches and completes a single job")
{
    auto exec = makePriorityExecutor(2);
    std::atomic<bool> ran{false};

    exec->dispatch("job", [&] { ran = true; }, nullptr, -1, 5);
    exec->wait_all();

    CHECK(ran.load());
    CHECK(exec->concurrency() == 2);
}

TEST_CASE("PriorityExecutor: higher-priority job queued behind a busy pool runs before lower-priority ones")
{
    // Single worker thread: the first dispatched job occupies it immediately,
    // so every subsequent dispatch queues up and priority ordering among the
    // queued jobs becomes observable (and deterministic) once the pool frees up.
    auto exec = makePriorityExecutor(1);

    std::mutex              mtx;
    std::condition_variable holdCv;
    bool                    releaseHold = false;

    // Occupies the single worker thread until the test explicitly releases it,
    // giving the test time to queue every other job below before any of them run.
    exec->dispatch("hold", [&] {
        std::unique_lock lk{mtx};
        holdCv.wait(lk, [&] { return releaseHold; });
    }, nullptr, -1, 1);

    std::vector<std::string> order;
    std::mutex               orderMtx;
    auto recordJob = [&](std::string_view name) {
        std::lock_guard lk{orderMtx};
        order.emplace_back(name);
    };

    exec->dispatch("low_a", [&] { recordJob("low_a"); }, nullptr, -1, 1);
    exec->dispatch("low_b", [&] { recordJob("low_b"); }, nullptr, -1, 1);
    exec->dispatch("high", [&] { recordJob("high"); }, nullptr, -1, 10);

    {
        std::lock_guard lk{mtx};
        releaseHold = true;
    }
    holdCv.notify_one();
    exec->wait_all();

    REQUIRE(order.size() == 3U);
    CHECK(order[0] == "high"); // highest priority among the three queued jobs
}

TEST_CASE("PriorityExecutor: equal-priority jobs run in dispatch order (stable FIFO)")
{
    // Same single-worker hold trick: occupy the one thread, queue several jobs at
    // the SAME priority, then release. A plain max-heap keyed on priority alone
    // would pop equal-priority jobs in arbitrary heap order; the enqueue-sequence
    // tie-break makes that order FIFO, which callers that dispatch an ordered
    // sequence (e.g. an access-order prefetch) depend on.
    auto exec = makePriorityExecutor(1);

    std::mutex              mtx;
    std::condition_variable holdCv;
    bool                    releaseHold = false;
    exec->dispatch("hold", [&] {
        std::unique_lock lk{mtx};
        holdCv.wait(lk, [&] { return releaseHold; });
    }, nullptr, -1, 5);

    std::vector<std::string> order;
    std::mutex               orderMtx;
    const std::vector<std::string> expected{"a", "b", "c", "d", "e"};
    for (const auto& name : expected) {
        exec->dispatch(name, [&, name] {
            std::lock_guard lk{orderMtx};
            order.push_back(name);
        }, nullptr, -1, 5);   // all identical priority
    }

    {
        std::lock_guard lk{mtx};
        releaseHold = true;
    }
    holdCv.notify_one();
    exec->wait_all();

    CHECK(order == expected);
}

TEST_CASE("PriorityExecutor: onThreadStart runs exactly once per worker, before any job on that thread")
{
    constexpr unsigned int kThreads = 3;
    std::atomic<unsigned int> startCount{0};

    std::mutex                 seenMtx;
    std::set<std::thread::id>  primedThreads;

    auto exec = makePriorityExecutor(kThreads, [&] {
        startCount.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lk{seenMtx};
        primedThreads.insert(std::this_thread::get_id());
    });

    // A single fast dispatch batch does not reliably exercise every worker --
    // one thread can win the whole queue before the OS schedules the others
    // (observed: 24 near-instant jobs all landing on one thread). A rendezvous
    // barrier forces genuine concurrency: every one of the kThreads jobs below
    // must be running simultaneously (one per worker) before any can proceed,
    // which is only possible once every worker thread has actually started.
    std::mutex              barrierMtx;
    std::condition_variable barrierCv;
    unsigned int            arrived = 0;
    std::atomic<unsigned int> jobsOnUnprimedThread{0};

    for (unsigned int i = 0; i < kThreads; ++i) {
        exec->dispatch("job", [&] {
            {
                std::lock_guard lk{seenMtx};
                if (!primedThreads.contains(std::this_thread::get_id()))
                    jobsOnUnprimedThread.fetch_add(1, std::memory_order_relaxed);
            }
            std::unique_lock lk{barrierMtx};
            if (++arrived == kThreads) {
                barrierCv.notify_all();
            } else {
                barrierCv.wait(lk, [&] { return arrived == kThreads; });
            }
        }, nullptr, -1, 5);
    }
    exec->wait_all();

    CHECK(startCount.load() == kThreads);
    CHECK(jobsOnUnprimedThread.load() == 0U);
}

TEST_CASE("LeanPriorityExecutor: dispatches, completes, and still honours priority")
{
    // The lean policy drops FIFO among equal-priority jobs but must still run and
    // still start a higher-priority job before a lower-priority queued one.
    auto exec = makeLeanPriorityExecutor(1);

    std::mutex              mtx;
    std::condition_variable holdCv;
    bool                    releaseHold = false;
    exec->dispatch("hold", [&] {
        std::unique_lock lk{mtx};
        holdCv.wait(lk, [&] { return releaseHold; });
    }, nullptr, -1, 1);

    std::vector<std::string> order;
    std::mutex               orderMtx;
    auto record = [&](std::string_view name) {
        std::lock_guard lk{orderMtx};
        order.emplace_back(name);
    };
    exec->dispatch("low",  [&] { record("low"); },  nullptr, -1, 1);
    exec->dispatch("high", [&] { record("high"); }, nullptr, -1, 10);

    {
        std::lock_guard lk{mtx};
        releaseHold = true;
    }
    holdCv.notify_one();
    exec->wait_all();

    REQUIRE(order.size() == 2U);
    CHECK(order[0] == "high");
}

TEST_CASE("PriorityExecutor: default (no onThreadStart) still dispatches correctly")
{
    auto exec = makePriorityExecutor(2);
    std::atomic<int> completed{0};

    for (int i = 0; i < 10; ++i)
        exec->dispatch("job", [&] { completed.fetch_add(1, std::memory_order_relaxed); }, nullptr, -1, 5);
    exec->wait_all();

    CHECK(completed.load() == 10);
}
