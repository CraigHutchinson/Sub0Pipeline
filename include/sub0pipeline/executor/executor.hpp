// include/sub0pipeline/executor/executor.hpp
//
// IExecutor — platform-injectable execution backend interface.
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace sub0pipeline {

// ── Executor interface ────────────────────────────────────────────────────────

/**
 * @brief Platform-injectable execution backend.
 *
 * Provides an abstraction layer so the same Pipeline DAG engine runs on
 * any platform: threaded, sequential/inline, or RTOS-based.
 *
 * Contract:
 *   - dispatch() MUST increment its in-flight counter before returning.
 *   - dispatch() MUST eventually call onComplete() from the dispatched context.
 *   - wait_all() MUST NOT return until all dispatched bodies and onComplete() calls have returned.
 */
class IExecutor
{
public:
    virtual ~IExecutor() = default;

    /**
     * @brief Dispatch a job for asynchronous execution.
     * @param name         Human-readable label (for logging).
     * @param fn           The job function to execute.
     * @param onComplete   Callback fired when fn returns (required by contract).
     * @param coreAffinity CPU core hint (-1 = any).
     * @param priority     Scheduling priority (1–24).
     * @param stackBytes   Stack allocation for embedded targets.
     */
    virtual void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           coreAffinity,
        uint8_t                       priority,
        uint32_t                      stackBytes) = 0;

    /** Block until dispatched jobs and their completion callbacks have returned. */
    virtual void wait_all() = 0;

    /** @return Number of parallel execution slots (cores / thread pool size). */
    [[nodiscard]] virtual int concurrency() const noexcept = 0;

    /**
     * @return true if dispatch() runs the job to completion on the calling
     *         thread before it returns.
     *
     * Pipeline::run() then calls ready jobs itself, in the order they become
     * ready, and does not use dispatch(). Stack depth stays constant however
     * long a dependency chain is; an executor that ran each job from inside
     * dispatch() would nest one call per link. Core affinity, priority and
     * stack hints do not apply to such a run. Pipeline::trigger() still goes
     * through dispatch().
     */
    [[nodiscard]] virtual bool runs_inline() const noexcept { return false; }
};

} // namespace sub0pipeline
