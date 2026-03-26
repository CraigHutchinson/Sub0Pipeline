// platform/esp32p4/freertos_executor.cpp
//
// FreeRTOS-based executor for Sub0Pipeline on ESP32-P4 dual-core RISC-V.
// Each dispatched job runs as a pinned FreeRTOS task with configurable
// priority and core affinity. Tasks self-delete on completion.

#include <sub0pipeline/sub0pipeline.hpp>

#include <atomic>
#include <memory>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char* kTag = "sub0pipeline";

namespace sub0pipeline {

class FreeRtosExecutor final : public IExecutor
{
public:
    FreeRtosExecutor()
    {
        completionSem_ = xSemaphoreCreateCounting(256, 0);
    }

    ~FreeRtosExecutor() override
    {
        if (completionSem_) vSemaphoreDelete(completionSem_);
    }

    void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         on_complete,
        int                           core_affinity,
        uint8_t                       priority,
        uint32_t                      stack_bytes) override
    {
        inFlight_.fetch_add(1U, std::memory_order_relaxed);

        // Heap-allocate the context — the task outlives this stack frame.
        struct Ctx
        {
            std::function<void()>  fn;
            std::function<void()>  on_complete;
            SemaphoreHandle_t      sem;
            std::atomic<uint32_t>* inFlight;
        };

        auto* ctx = new (std::nothrow) Ctx{
            std::move(fn), std::move(on_complete), completionSem_, &inFlight_};

        if (!ctx) {
            ESP_LOGE(kTag, "dispatch alloc failed for '%.*s' (free_heap=%lu)",
                     static_cast<int>(name.size()), name.data(),
                     static_cast<unsigned long>(xPortGetFreeHeapSize()));
            // Run synchronously as fallback to avoid stalling the pipeline.
            fn();
            if (on_complete) on_complete();
            inFlight_.fetch_sub(1U, std::memory_order_release);
            xSemaphoreGive(completionSem_);
            return;
        }

        // Task name: truncate to 15 chars (FreeRTOS limit).
        char taskName[16]{};
        const auto len = std::min<std::size_t>(name.size(), 15U);
        std::copy_n(name.data(), len, taskName);

        const BaseType_t core = (core_affinity >= 0 && core_affinity <= 1)
            ? static_cast<BaseType_t>(core_affinity)
            : tskNO_AFFINITY;

        const BaseType_t rc = xTaskCreatePinnedToCore(
            [](void* arg)
            {
                auto* c = static_cast<Ctx*>(arg);
                c->fn();
                if (c->on_complete) c->on_complete();
                c->inFlight->fetch_sub(1U, std::memory_order_release);
                xSemaphoreGive(c->sem);
                delete c;
                vTaskDelete(nullptr);
            },
            taskName,
            stack_bytes,
            ctx,
            priority,
            nullptr,
            core);

        if (rc != pdPASS) {
            ESP_LOGE(kTag, "xTaskCreate failed for '%s' (stack=%lu, free_heap=%lu)",
                     taskName,
                     static_cast<unsigned long>(stack_bytes),
                     static_cast<unsigned long>(xPortGetFreeHeapSize()));
            // Run synchronously as a fallback so the pipeline can propagate
            // to successors rather than silently stalling.
            ctx->fn();
            if (ctx->on_complete) ctx->on_complete();
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
        return 2;  // ESP32-P4 dual-core RISC-V
    }

private:
    SemaphoreHandle_t     completionSem_{nullptr};
    std::atomic<uint32_t> inFlight_{0U};
};

/** @return A FreeRtosExecutor for ESP32-P4 dual-core task dispatch. */
std::unique_ptr<IExecutor> make_freertos_executor()
{
    return std::make_unique<FreeRtosExecutor>();
}

} // namespace sub0pipeline
