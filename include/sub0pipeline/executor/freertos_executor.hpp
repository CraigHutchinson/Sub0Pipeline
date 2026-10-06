// include/sub0pipeline/executor/freertos_executor.hpp
//
// FreeRtosExecutor — one FreeRTOS task per job.
// Built only as the esp32p4 ESP-IDF component; this header itself needs no
// FreeRTOS headers, so it can be included on any host.
#pragma once

#include <sub0pipeline/executor/executor.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string_view>

namespace sub0pipeline {

/**
 * @brief Executor that runs each job as its own FreeRTOS task.
 *
 * Honours the job's core affinity (cores 0 and 1; anything else is unpinned),
 * priority (clamped to 1–24) and stack size. Each task deletes itself when its
 * job finishes. If the task or its context cannot be created, the job runs
 * synchronously on the dispatching task so the pipeline still makes progress.
 *
 * Owns one counting semaphore, created in the constructor. Construct it
 * wherever suits the caller (stack, member, static); it is neither copyable
 * nor movable. Call wait_all() before destroying it: the destructor does not
 * wait for running tasks. Task context only; not for use from an ISR.
 */
class FreeRtosExecutor final : public IExecutor
{
public:
    /** Creates the completion semaphore. */
    FreeRtosExecutor();

    /** Deletes the completion semaphore. Does not wait for running tasks. */
    ~FreeRtosExecutor() override;

    FreeRtosExecutor(const FreeRtosExecutor&)            = delete;
    FreeRtosExecutor& operator=(const FreeRtosExecutor&) = delete;

    void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           coreAffinity,
        uint8_t                       priority,
        uint32_t                      stackBytes) override;

    void wait_all() override;

    /** @return The number of processor cores FreeRTOS schedules on. */
    [[nodiscard]] int concurrency() const noexcept override;

private:
    void*                 completionSem_{nullptr}; ///< owning; a FreeRTOS SemaphoreHandle_t
    std::atomic<uint32_t> inFlight_{0U};
};

} // namespace sub0pipeline
