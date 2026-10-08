#include <atomic>
#include <chrono>
#include <condition_variable>
#include <doctest.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "sub0pipeline/executor/freertos_executor.hpp"

using namespace sub0pipeline;

struct MockSemaphore
{
    std::mutex mutex_;
    std::condition_variable ready_;
    unsigned int signals_{0};
};

namespace
{
struct KernelFixture;
KernelFixture* kernel = nullptr; // non-owning, single scoped host kernel
thread_local bool failContextAllocation = false;

struct KernelFixture
{
    KernelFixture() { kernel = this; }
    ~KernelFixture() { join(); kernel = nullptr; }
    void join() { workers_.clear(); }

    MockSemaphore semaphore_;
    bool failSemaphore_{false};
    bool failTask_{false};
    int deleted_{0};
    int taskCalls_{0};
    std::vector<std::jthread> workers_;
    std::function<void()> onGive_;
    std::function<bool(int)> onTake_;
    std::atomic<int> takeCalls_{0};
};

struct ThrowingCopy
{
    bool& armed_;
    int& calls_;
    ThrowingCopy(bool& armed, int& calls) noexcept : armed_{armed}, calls_{calls} {}
    ThrowingCopy(const ThrowingCopy& other) : armed_{other.armed_}, calls_{other.calls_}
    {
        if (armed_) throw std::bad_alloc{};
    }
    void operator()() const noexcept { ++calls_; }
};
}

std::size_t xPortGetFreeHeapSize() { return 1024U * 1024U; }
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned int, unsigned int)
{
    return kernel->failSemaphore_ ? nullptr : &kernel->semaphore_;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    CHECK(semaphore == &kernel->semaphore_);
    ++kernel->deleted_;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (kernel->onGive_) kernel->onGive_();
    std::lock_guard lock{semaphore->mutex_};
    ++semaphore->signals_;
    semaphore->ready_.notify_all();
    return pdPASS;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    const int call = ++kernel->takeCalls_;
    if (kernel->onTake_ && kernel->onTake_(call)) return pdFAIL;
    std::unique_lock lock{semaphore->mutex_};
    if (!semaphore->ready_.wait_for(lock, std::chrono::milliseconds{timeout},
                                   [&] { return semaphore->signals_ != 0U; })) return pdFAIL;
    --semaphore->signals_;
    return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char*, uint32_t,
    void* context, uint8_t, void**, BaseType_t)
{
    ++kernel->taskCalls_;
    if (kernel->failTask_) return pdFAIL;
    kernel->workers_.emplace_back([=] { function(context); });
    return pdPASS;
}
void vTaskDelete(void*) {}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (std::exchange(failContextAllocation, false)) return nullptr;
    try { return ::operator new(size); }
    catch (...) { return nullptr; }
}
void operator delete(void* storage, const std::nothrow_t&) noexcept { ::operator delete(storage); }

TEST_CASE("FreeRTOS submission: invalid semaphore construction rejects")
{
    KernelFixture fixture;
    fixture.failSemaphore_ = true;
    CHECK_THROWS_AS(FreeRtosExecutor{}, std::runtime_error);
    CHECK(fixture.taskCalls_ == 0);
    CHECK(fixture.deleted_ == 0);
}

TEST_CASE("FreeRTOS submission: callable-copy rejection preserves wait and reuse")
{
    KernelFixture fixture;
    FreeRtosExecutor executor;
    bool armed = false;
    int calls = 0;
    std::function<void()> body = ThrowingCopy{armed, calls};
    armed = true;
    CHECK_THROWS_AS(executor.dispatch("copy", std::move(body), [] {}, -1, 5, 2048), std::bad_alloc);
    executor.waitAll();
    CHECK(fixture.taskCalls_ == 0);
    CHECK(calls == 0);
    executor.dispatch("recover", [&] { ++calls; }, [] {}, -1, 5, 2048);
    executor.waitAll();
    fixture.join();
    CHECK(calls == 1);
}

TEST_CASE("FreeRTOS submission: task and context exhaustion retain inline completion")
{
    for (const bool failContext : {false, true})
    {
        KernelFixture fixture;
        FreeRtosExecutor executor;
        fixture.failTask_ = !failContext;
        int bodies = 0, completions = 0;
        failContextAllocation = failContext;
        executor.dispatch("fallback", [&] { ++bodies; }, [&] { ++completions; }, -1, 5, 2048);
        executor.waitAll();
        CHECK_FALSE(failContextAllocation);
        CHECK(bodies == 1);
        CHECK(completions == 1);
        CHECK(fixture.taskCalls_ == (failContext ? 0 : 1));
    }
}

TEST_CASE("FreeRTOS submission: captures and semaphore finish before publication permits teardown")
{
    KernelFixture fixture;
    std::latch bodyEntered{1}, releaseBody{1}, firstTake{1}, forceTimeout{1};
    std::latch giveEntered{1}, releaseGive{1}, secondTake{1};
    std::atomic<bool> reclaimed{false}, returned{false};
    auto payload = std::make_shared<int>(17);
    const std::weak_ptr<int> retained = payload;
    fixture.onGive_ = [&]
    {
        reclaimed = retained.expired();
        giveEntered.count_down();
        releaseGive.wait();
    };
    fixture.onTake_ = [&](int call)
    {
        if (call == 1)
        {
            firstTake.count_down();
            forceTimeout.wait();
            return true;
        }
        if (call == 2) secondTake.count_down();
        return false;
    };
    {
        FreeRtosExecutor executor;
        executor.dispatch("held", [&, payload] { bodyEntered.count_down(); releaseBody.wait(); },
                          [payload] {}, -1, 5, 2048);
        payload.reset();
        bodyEntered.wait();
        std::jthread waiter{[&] { executor.waitAll(); returned = true; }};
        firstTake.wait();
        releaseBody.count_down();
        giveEntered.wait();
        CHECK(reclaimed.load());
        forceTimeout.count_down();
        secondTake.wait();
        CHECK_FALSE(returned.load());
        CHECK(fixture.deleted_ == 0);
        releaseGive.count_down();
        waiter.join();
    }
    // Owner teardown precedes the kernel's self-delete tail; no borrowed owner remains there.
    fixture.join();
    CHECK(fixture.deleted_ == 1);
    CHECK(returned.load());
}
