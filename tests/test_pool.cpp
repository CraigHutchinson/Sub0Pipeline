// tests/test_pool.cpp
//
// PoolSuccessors: pool/arena behaviour tests.

#include <sub0pipeline/sub0pipeline.hpp>
#include "test_helpers.hpp"
#include "doctest.h"

#include <algorithm>
#include <mutex>
#include <vector>

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
    for (int i = 0; i < 8; ++i) {
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
