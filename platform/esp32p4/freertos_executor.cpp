// platform/esp32p4/freertos_executor.cpp
//
// FreeRTOS-based executor for Sub0Pipeline on ESP32-P4 dual-core RISC-V.
// Each dispatched job runs as a pinned FreeRTOS task with configurable
// priority and core affinity. Tasks self-delete on completion.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <functional>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

#include "sub0pipeline/config.hpp"
#include "sub0pipeline/executor/freertos_executor.hpp"

static constexpr const char* cTag = "sub0pipeline";

namespace sub0pipeline
{

namespace
{
// The public header stores the handle as void* so that it needs no FreeRTOS headers.
SemaphoreHandle_t semaphore(void* handle) noexcept
{
    return static_cast<SemaphoreHandle_t>(handle);
}
} // namespace

FreeRtosExecutor::FreeRtosExecutor()
{
    completionSem_ = xSemaphoreCreateCounting(0x7FFFFFFF, 0);
    if (!completionSem_)
    {
        SUB0PIPELINE_THROW("FreeRtosExecutor completion semaphore creation failed");
    }
}

FreeRtosExecutor::~FreeRtosExecutor()
{
    if (completionSem_) vSemaphoreDelete(semaphore(completionSem_));
}

void FreeRtosExecutor::dispatch(
    std::string_view              name,
    std::function<void()>         fn,
    std::function<void()>         onComplete,
    int                           coreAffinity,
    uint8_t                       priority,
    uint32_t                      stackBytes)
{
    const uint8_t clampedPriority = std::clamp<uint8_t>(priority, 1U, 24U);

    // Heap-allocate the context — the task outlives this stack frame.
    struct Ctx
    {
        std::function<void()>  fn;
        std::function<void()>  onComplete;
        SemaphoreHandle_t      sem;
        std::atomic<uint32_t>* inFlight; // non-owning; caller joins before executor teardown

        static void execute(void* raw)
        {
            std::unique_ptr<Ctx> context{static_cast<Ctx*>(raw)};
            context->fn();
            if (context->onComplete) context->onComplete();
            const auto sem = context->sem;
            auto* count = context->inFlight;
            context.reset();
            // No semaphore or borrowed callable remains in use after publication.
            xSemaphoreGive(sem);
            count->fetch_sub(1U, std::memory_order_release);
        }
    };

    // Keep copies for the fallback path before moving into ctx.
    auto fnCopy         = fn;
    auto onCompleteCopy = onComplete;

    auto* ctx = new (std::nothrow) Ctx{
        std::move(fn), std::move(onComplete), semaphore(completionSem_), &inFlight_};

    if (!ctx)
    {
        ESP_LOGE(cTag, "dispatch alloc failed for '%.*s' (free_heap=%lu)",
                 static_cast<int>(name.size()), name.data(),
                 static_cast<unsigned long>(xPortGetFreeHeapSize()));
        // Run synchronously as fallback to avoid stalling the pipeline.
        inFlight_.fetch_add(1U, std::memory_order_relaxed);
        fnCopy();
        if (onCompleteCopy) onCompleteCopy();
        fn = {};
        onComplete = {};
        fnCopy = {};
        onCompleteCopy = {};
        xSemaphoreGive(semaphore(completionSem_));
        inFlight_.fetch_sub(1U, std::memory_order_release);
        return;
    }

    fnCopy = {};
    onCompleteCopy = {};
    inFlight_.fetch_add(1U, std::memory_order_relaxed);

    // Task name: truncate to 15 chars (FreeRTOS limit).
    char taskName[16]{};
    const auto len = std::min<std::size_t>(name.size(), 15U);
    std::copy_n(name.data(), len, taskName);

    const BaseType_t core = (coreAffinity >= 0 && coreAffinity <= 1)
        ? static_cast<BaseType_t>(coreAffinity)
        : tskNO_AFFINITY;

    const BaseType_t rc = xTaskCreatePinnedToCore(
        [](void* arg)
        {
            Ctx::execute(arg);
            vTaskDelete(nullptr);
        },
        taskName,
        stackBytes,
        ctx,
        clampedPriority,
        nullptr,
        core);

    if (rc != pdPASS)
    {
        ESP_LOGE(cTag, "xTaskCreate failed for '%s' (stack=%lu, free_heap=%lu)",
                 taskName,
                 static_cast<unsigned long>(stackBytes),
                 static_cast<unsigned long>(xPortGetFreeHeapSize()));
        // Run synchronously as a fallback so the pipeline can propagate
        // to successors rather than silently stalling.
        Ctx::execute(ctx);
    }
}

void FreeRtosExecutor::waitAll()
{
    // Drain: block until all in-flight tasks have completed.
    while (inFlight_.load(std::memory_order_acquire) > 0U)
    {
        xSemaphoreTake(semaphore(completionSem_), pdMS_TO_TICKS(100));
    }
}

int FreeRtosExecutor::concurrency() const noexcept
{
#ifdef portNUM_PROCESSORS
    return portNUM_PROCESSORS;
#else
    return 2;  // ESP32-P4 dual-core RISC-V
#endif
}

} // namespace sub0pipeline
