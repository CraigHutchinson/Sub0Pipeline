// include/sub0pipeline/executor/executor.hpp
//
// IExecutor — platform-injectable execution backend interface.
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace sub0pipeline
{

// ── Executor interface ────────────────────────────────────────────────────────

/**
 * Abstracts the execution backend so each platform can inject its own.
 *
 * Provides an abstraction layer so the same Pipeline DAG engine runs on
 * any platform: threaded, sequential/inline, or RTOS-based.
 *
 * Contract:
 *   - Accepted dispatches MUST be accounted for before returning, unless the
 *     body and completion already returned inline; no public counter is required.
 *   - Accepted dispatches MUST eventually call onComplete() from their context.
 *   - If dispatch() throws, no job was accepted: neither callback is retained
 *     or invoked. Exceptions while constructing its arguments also reject.
 *   - waitAll() MUST NOT return until all dispatched bodies and onComplete() calls have returned.
 *     It MUST NOT throw while accepted callbacks can still execute.
 *     Accepted body/completion targets are destroyed before publishing completion;
 *     body-owned state remains alive until the completion target is destroyed.
 *   - Job bodies, completion callbacks and callable destructors MUST NOT throw.
 */
class IExecutor
{
public:
    virtual ~IExecutor() = default;

    /**
     * Dispatch a job for asynchronous execution.
     * @param name         Human-readable label (for logging).
     * @param fn           The job function to execute.
     * @param onComplete   Callback fired when fn returns (required by contract).
     * @param coreAffinity CPU core hint (-1 = any).
     * @param priority     Scheduling priority (1–24).
     * @param stackBytes   Stack allocation for embedded targets.
     * @throws Submission exceptions reject the job without accepting work.
     */
    virtual void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           coreAffinity,
        uint8_t                       priority,
        uint32_t                      stackBytes) = 0;

    /** Block until dispatched jobs and their completion callbacks have returned. */
    virtual void waitAll() = 0;

    /**
     * Reports how many jobs can run in parallel.
     * @return Number of parallel execution slots (cores / thread pool size).
     */
    [[nodiscard]] virtual int concurrency() const noexcept = 0;

    /**
     * Reports whether dispatch() runs each job on the calling thread.
     *
     * Pipeline::run() then calls ready jobs itself, in the order they become
     * ready, and does not use dispatch(). Stack depth stays constant however
     * long a dependency chain is; an executor that ran each job from inside
     * dispatch() would nest one call per link. Core affinity, priority and
     * stack hints do not apply to such a run. Pipeline::trigger() still goes
     * through dispatch().
     *
     * @return true if dispatch() runs the job to completion on the calling
     *         thread before it returns.
     */
    [[nodiscard]] virtual bool runsInline() const noexcept { return false; }
};

} // namespace sub0pipeline
