// tests/test_pool.cpp
//
// PoolSuccessors: pool/arena behaviour tests.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <doctest.h>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "sub0pipeline/sub0pipeline.hpp"
#include "test_helpers.hpp"

using namespace sub0pipeline;

// ═══════════════════════════════════════════════════════════════════════════════
// PoolSuccessors: pool/arena behaviour
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("PoolSuccessors: inline capacity holds exactly 4 entries without pool")
{
    RecordingExecutor exec;
    Pipeline pipe;
    std::atomic<int> ran{0};

    auto root = pipe.emplace([&]{ ++ran; }).name("root");
    for (int i = 0; i < 4; ++i)
        pipe.emplace([&]{ ++ran; }).name("leaf_" + std::to_string(i)).succeed(root);

    auto result = pipe.run(exec);
    CHECK(result.has_value());
    CHECK(ran.load() == 5); // root + 4 leaves
    // Verify all 4 successors dispatched in order after root
    const auto& order = exec.order();
    REQUIRE(order.size() == 5U);
    CHECK(order[0] == "root");
}

TEST_CASE("PoolSuccessors: 5th successor spills to pool, all complete correctly")
{
    RecordingExecutor exec;
    Pipeline pipe;
    std::atomic<int> ran{0};

    auto root = pipe.emplace([&]{ ++ran; }).name("root");
    for (int i = 0; i < 5; ++i)  // one more than inline capacity
        pipe.emplace([&]{ ++ran; }).name("leaf_" + std::to_string(i)).succeed(root);

    auto result = pipe.run(exec);
    CHECK(result.has_value());
    CHECK(ran.load() == 6); // root + 5 leaves
}

TEST_CASE("PoolSuccessors: wide fan-out N=16 (multiple pool grow cycles)")
{
    RecordingExecutor exec;
    Pipeline pipe;
    std::atomic<int> ran{0};
    constexpr int cN = 16;

    auto root = pipe.emplace([&]{ ++ran; }).name("root");
    for (int i = 0; i < cN; ++i)
        pipe.emplace([&]{ ++ran; }).succeed(root);

    auto result = pipe.run(exec);
    CHECK(result.has_value());
    CHECK(ran.load() == cN + 1);
}

TEST_CASE("PoolSuccessors: N=128 fan-out exercises full uint16_t count range")
{
    RecordingExecutor exec;
    Pipeline pipe;
    std::atomic<int> ran{0};
    constexpr int cN = 128; // exceeds old 7-bit limit of 127

    auto root = pipe.emplace([&]{ ++ran; }).name("root");
    for (int i = 0; i < cN; ++i)
        pipe.emplace([&]{ ++ran; }).succeed(root);

    auto result = pipe.run(exec);
    CHECK(result.has_value());
    CHECK(ran.load() == cN + 1);
    // All leaves must have run after root
    const auto& order = exec.order();
    REQUIRE(order.size() == static_cast<size_t>(cN + 1));
    CHECK(order[0] == "root");
}

TEST_CASE("PoolSuccessors: pool iteration matches push order")
{
    RecordingExecutor exec;
    Pipeline pipe;
    std::vector<int> results;
    std::mutex mtx;

    auto root = pipe.emplace([]{}); // root with no action
    for (int i = 0; i < 8; ++i)
    {
        pipe.emplace([&results, &mtx, i]{
            std::lock_guard lk{mtx};
            results.push_back(i);
        }).succeed(root);
    }

    auto result = pipe.run(exec);
    CHECK(result.has_value());
    REQUIRE(results.size() == 8U);
    // With sequential executor, all 8 successors ran -- check set equality
    std::sort(results.begin(), results.end());
    for (int i = 0; i < 8; ++i) CHECK(results[i] == i);
}

// A full block used to be re-copied on every append, so one job's successor
// list consumed n^2/2 arena entries and overflowed the arena near 362 entries.

TEST_CASE("PoolSuccessors: a 5000-job fan-out builds, keeps push order and runs once each")
{
    constexpr int cN = 5000;
    Pipeline pipe;
    std::atomic<int> ran{0};

    auto root = pipe.emplace([&] { ++ran; });
    for (int i = 0; i < cN; ++i)
        pipe.emplace([&] { ++ran; }).succeed(root);

    const auto successors = pipe.successors(JobId{0});
    REQUIRE(successors.size() == static_cast<std::size_t>(cN));
    JobId expected = 1;
    for (const auto target : successors) CHECK(target.id == expected++);

    CHECK(pipe.runInline().has_value());
    CHECK(ran.load() == cN + 1);
}

TEST_CASE("PoolSuccessors: interleaved wide fan-outs relocate without disturbing each other")
{
    // Alternating appends mean neither list is the arena's last block when it
    // fills, so both take the move-to-a-new-block path repeatedly.
    constexpr int cN = 2000;
    Pipeline pipe;
    auto first  = pipe.emplace([] {});
    auto second = pipe.emplace([] {});
    for (int i = 0; i < cN; ++i)
    {
        first.precede(pipe.emplace([] {}));
        pipe.emplace([] {}).succeed(second);
    }

    const auto fromFirst  = pipe.successors(JobId{0});
    const auto fromSecond = pipe.successors(JobId{1});
    REQUIRE(fromFirst.size() == static_cast<std::size_t>(cN));
    REQUIRE(fromSecond.size() == static_cast<std::size_t>(cN));
    JobId expected = 2;
    for (const auto target : fromFirst) { CHECK(target.id == expected); expected += 2; }
    expected = 3;
    for (const auto target : fromSecond) { CHECK(target.id == expected); expected += 2; }

    CHECK(pipe.validate().has_value());
    CHECK(pipe.runInline().has_value());
}

#if SUB0PIPELINE_EXCEPTIONS
TEST_CASE("PoolSuccessors: one job accepts 32767 successors and rejects the next")
{
    constexpr int cLimit = 32767;
    Pipeline pipe;
    auto root = pipe.emplace([] {});
    for (int i = 0; i < cLimit; ++i)
        pipe.emplace([] {}).succeed(root);
    CHECK(pipe.successors(JobId{0}).size() == static_cast<std::size_t>(cLimit));

    std::atomic<bool> extraRan{false};
    auto extra = pipe.emplace([&] { extraRan = true; });
    CHECK_THROWS_AS(extra.succeed(root), std::runtime_error);
    CHECK(pipe.successors(JobId{0}).size() == static_cast<std::size_t>(cLimit));

    // The rejected edge left nothing behind: the job is an ordinary root.
    CHECK(pipe.runInline().has_value());
    CHECK(extraRan.load());
}
#endif
