#include <atomic>
#include <cstdlib>
#include <doctest.h>
#include <functional>
#include <latch>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "sub0pipeline/sub0pipeline.hpp"

using namespace sub0pipeline;

namespace
{
struct AllocationFailure
{
    bool next_{false};
    bool all_{false};
    std::size_t minimumBytes_{0U};
    std::size_t attempts_{0U};
    std::size_t denied_{0U};
};

// The isolated process redirects ordinary new only on the injecting thread.
thread_local AllocationFailure* allocationFailure = nullptr; // non-owning, scoped below

struct AllocationWindow
{
    explicit AllocationWindow(AllocationFailure& failure) noexcept { allocationFailure = &failure; }
    ~AllocationWindow() { allocationFailure = nullptr; }
};

void* allocate(std::size_t size)
{
    if (allocationFailure)
    {
        ++allocationFailure->attempts_;
        const bool failNext = allocationFailure->next_ && size >= allocationFailure->minimumBytes_;
        if (failNext) allocationFailure->next_ = false;
        if (failNext || allocationFailure->all_)
        {
            ++allocationFailure->denied_;
            throw std::bad_alloc{};
        }
    }
    if (auto* storage = std::malloc(size != 0U ? size : 1U)) return storage;
    throw std::bad_alloc{};
}

/** Synchronous accepted work makes persistent failure injection deterministic. */
class AllocationRejectingExecutor final : public IExecutor
{
public:
    explicit AllocationRejectingExecutor(std::string_view rejectedName) noexcept
        : rejectedName_{rejectedName} {}

    void dispatch(std::string_view name, std::function<void()> fn,
                  std::function<void()> complete, int, uint8_t, uint32_t) override
    {
        if (name == rejectedName_)
        {
            allocationFailure->all_ = true;
            throw std::bad_alloc{};
        }
        ++accepted_;
        fn();
        if (complete) complete();
    }

    void waitAll() override
    {
        if (allocationFailure) allocationFailure->all_ = false;
        ++waits_;
    }

    [[nodiscard]] int concurrency() const noexcept override { return 1; }
    int accepted_{0};
    int waits_{0};

private:
    std::string_view rejectedName_;
};
}

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* storage) noexcept { std::free(storage); }
void operator delete[](void* storage) noexcept { std::free(storage); }
void operator delete(void* storage, std::size_t) noexcept { std::free(storage); }
void operator delete[](void* storage, std::size_t) noexcept { std::free(storage); }

TEST_CASE("Submission allocation: bounded queue reserve fails before starting a worker")
{
    int started = 0;
    PriorityExecutor::Options options{.threadCount = 1, .onThreadStart = [&] { ++started; },
                                      .queueCapacity = 256};
    // MSVC Debug vector construction allocates a small iterator proxy inside
    // its noexcept constructor. Target the queued storage, not that STL boundary.
    AllocationFailure failure{.next_ = true, .minimumBytes_ = 1024U};
    bool rejected = false;
    {
        AllocationWindow window{failure};
        try { PriorityExecutor executor{std::move(options)}; }
        catch (const std::bad_alloc&) { rejected = true; }
    }
    CHECK(rejected);
    CHECK(failure.denied_ == 1U);
    CHECK(started == 0);
}

TEST_CASE("Submission allocation: queue growth rejection does not strand accepted work")
{
    PriorityExecutor executor{{.threadCount = 1}};
    std::latch entered{1}, release{1};
    std::atomic<int> bodies{0}, completions{0};
    executor.dispatch("held", [&] { ++bodies; entered.count_down(); release.wait(); },
                      [&] { ++completions; }, -1, 5, 0);
    entered.wait();
    AllocationFailure failure;
    bool rejected = false;
    std::size_t admitted = 0U;
    {
        AllocationWindow window{failure};
        // Each insertion is attempted without storage allocation. Eventually
        // the dynamically growing queue must grow while the sole worker waits.
        for (; admitted < 4096U && !rejected; ++admitted)
        {
            failure.next_ = true;
            try
            {
                executor.dispatch("queued", [&] { ++bodies; }, [&] { ++completions; }, -1, 5, 0);
            }
            catch (const std::bad_alloc&) { rejected = true; }
            failure.next_ = false;
        }
    }
    release.count_down();
    executor.waitAll();
    REQUIRE(rejected);
    // The loop counts the rejected attempt; the initially held job balances it.
    CHECK(bodies.load() == static_cast<int>(admitted));
    CHECK(completions.load() == static_cast<int>(admitted));
    executor.dispatch("reused", [&] { ++bodies; }, [&] { ++completions; }, -1, 5, 0);
    executor.waitAll();
    CHECK(bodies.load() == static_cast<int>(admitted) + 1);
}

TEST_CASE("Submission allocation: bounded queued storage is reused without ordinary caller new")
{
    PriorityExecutor executor{{.threadCount = 1, .queueCapacity = 8}};
    for (int pass = 0; pass < 16; ++pass)
    {
        std::latch entered{1}, release{1};
        std::atomic<int> completed{0};
        executor.dispatch("held", [&] { entered.count_down(); release.wait(); }, {}, -1, 5, 0);
        entered.wait();
        AllocationFailure failure;
        {
            AllocationWindow window{failure};
            for (int queued = 0; queued < 8; ++queued)
            {
                executor.dispatch("queued", [] {}, [&] { ++completed; }, -1, 5, 0);
            }
        }
        release.count_down();
        executor.waitAll();
        CHECK(failure.attempts_ == 0U);
        CHECK(completed.load() == 8);
    }
}

TEST_CASE("Submission allocation: scoped completion construction fails before accounting")
{
    AllocationRejectingExecutor parent{"unused"};
    ScopedExecutor scoped{parent};
    AllocationFailure failure{.next_ = true};
    bool rejected = false;
    {
        AllocationWindow window{failure};
        try { scoped.dispatch("scope", [] {}, [] {}, -1, 5, 0); }
        catch (const std::bad_alloc&) { rejected = true; }
    }
    scoped.waitAll();
    CHECK(rejected);
    CHECK(parent.accepted_ == 0);
    CHECK(failure.attempts_ == 1U);
    scoped.dispatch("recover", [] {}, [] {}, -1, 5, 0);
    scoped.waitAll();
    CHECK(parent.accepted_ == 1);
}

TEST_CASE("Submission allocation: cold graph contains root and successor bad_alloc without allocating")
{
    for (const bool successor : {false, true})
    {
        const std::string rejectedName(256U, 'r');
        Pipeline pipeline;
        auto root = pipeline.emplace([] {}).name(successor ? "root" : rejectedName);
        Job failed = root;
        if (successor)
        {
            failed = pipeline.emplace([] {}).name(rejectedName).optional();
            failed.succeed(root);
        }
        auto child = pipeline.emplace([] {}).name("child");
        child.succeed(failed);
        AllocationRejectingExecutor executor{rejectedName};
        AllocationFailure failure;
        std::expected<void, PipelineError> result;
        std::size_t attempts = 0U;
        {
            AllocationWindow window{failure};
            result = pipeline.run(executor);
            attempts = failure.attempts_;
        }
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == PipelineError::kJobFailed);
        CHECK(pipeline.firstFailureName() == rejectedName);
        CHECK(pipeline.status(failed) == JobStatus::kFailed);
        CHECK(pipeline.status(child) == JobStatus::kSkipped);
        CHECK(executor.waits_ == 1);
        // Initial graph preparation allocates before the injected rejection.
        CHECK(attempts > 0U);
        CHECK(failure.denied_ == 0U);
    }
}
