// platform/esp32p4/freertos_executor.cpp
//
// FreeRTOS-based executor for Sub0Pipeline on ESP32-P4 dual-core RISC-V.
// Each dispatched job runs as a pinned FreeRTOS task with configurable
// priority and core affinity. Tasks self-delete on completion.

#include <sub0pipeline/sub0pipeline.hpp>

#include <algorithm>
#include <atomic>
#include <memory>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static constexpr const char* cTag = "sub0pipeline";

namespace sub0pipeline {

class FreeRtosExecutor final : public IExecutor
{
public:
    FreeRtosExecutor()
    {
        completionSem_ = xSemaphoreCreateCounting(0x7FFFFFFF, 0);
    }

    ~FreeRtosExecutor() override
    {
        if (completionSem_) vSemaphoreDelete(completionSem_);
    }

    void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           coreAffinity,
        uint8_t                       priority,
        uint32_t                      stackBytes) override
    {
        inFlight_.fetch_add(1U, std::memory_order_relaxed);

        const uint8_t clampedPriority = std::clamp<uint8_t>(priority, 1U, 24U);

        // Heap-allocate the context — the task outlives this stack frame.
        struct Ctx
        {
            std::function<void()>  fn;
            std::function<void()>  onComplete;
            SemaphoreHandle_t      sem;
            std::atomic<uint32_t>* inFlight;
        };

        // Keep copies for the fallback path before moving into ctx.
        auto fnCopy         = fn;
        auto onCompleteCopy = onComplete;

        auto* ctx = new (std::nothrow) Ctx{
            std::move(fn), std::move(onComplete), completionSem_, &inFlight_};

        if (!ctx) {
            ESP_LOGE(cTag, "dispatch alloc failed for '%.*s' (free_heap=%lu)",
                     static_cast<int>(name.size()), name.data(),
                     static_cast<unsigned long>(xPortGetFreeHeapSize()));
            // Run synchronously as fallback to avoid stalling the pipeline.
            fnCopy();
            if (onCompleteCopy) onCompleteCopy();
            inFlight_.fetch_sub(1U, std::memory_order_release);
            xSemaphoreGive(completionSem_);
            return;
        }

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
                auto* c = static_cast<Ctx*>(arg);
                c->fn();
                if (c->onComplete) c->onComplete();
                c->inFlight->fetch_sub(1U, std::memory_order_release);
                xSemaphoreGive(c->sem);
                delete c;
                vTaskDelete(nullptr);
            },
            taskName,
            stackBytes,
            ctx,
            clampedPriority,
            nullptr,
            core);

        if (rc != pdPASS) {
            ESP_LOGE(cTag, "xTaskCreate failed for '%s' (stack=%lu, free_heap=%lu)",
                     taskName,
                     static_cast<unsigned long>(stackBytes),
                     static_cast<unsigned long>(xPortGetFreeHeapSize()));
            // Run synchronously as a fallback so the pipeline can propagate
            // to successors rather than silently stalling.
            ctx->fn();
            if (ctx->onComplete) ctx->onComplete();
            ctx->inFlight->fetch_sub(1U, std::memory_order_release);
            xSemaphoreGive(ctx->sem);
            delete ctx;
        }
    }

    void wait_all() override
    {
        // Drain: block until all in-flight tasks have completed.
        while (inFlight_.load(std::memory_order_acquire) > 0U) {
            xSemaphoreTake(completionSem_, pdMS_TO_TICKS(100));
        }
    }

    [[nodiscard]] int concurrency() const noexcept override
    {
#ifdef portNUM_PROCESSORS
        return portNUM_PROCESSORS;
#else
        return 2;  // ESP32-P4 dual-core RISC-V
#endif
    }

private:
    SemaphoreHandle_t     completionSem_{nullptr};
    std::atomic<uint32_t> inFlight_{0U};
};

/** @return A FreeRtosExecutor for ESP32-P4 dual-core task dispatch. */
std::unique_ptr<IExecutor> makeFreeRtosExecutor()
{
    return std::make_unique<FreeRtosExecutor>();
}

} // namespace sub0pipeline
