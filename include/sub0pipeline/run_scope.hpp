#pragma once

#include "sub0pipeline/sub0pipeline.hpp"
#include <thread>

namespace sub0pipeline
{

/**
 * Owns one run thread. Destruction requests stop and joins executor callbacks,
 * deadline callbacks and orphan workers before returning. Declare this AFTER
 * borrowed members; alternatively explicitly join before tearing those down.
 * Pipeline, executor, observer and deadline service must outlive the scope.
 * Do not join/destroy from its jobs, stop callbacks or a GUI thread needed by
 * the executor. Consumer callbacks must not throw, as with threaded executors.
 * No other run/trigger/mutation may overlap this scope. request_stop()/complete()
 * are thread-safe; join() is serialized. Never race destruction with API calls.
 */
class RunScope final
{
public:
    /**
     * Starts a thread that runs @p pipeline on @p executor.
     * @param pipeline  The graph to run. Borrowed; must outlive the scope.
     * @param executor  The execution backend. Borrowed; must outlive the scope.
     * @param observer  Optional observer, or nullptr. Borrowed; must outlive
     *                  the scope.
     */
    RunScope(Pipeline& pipeline, IExecutor& executor, IObserver* observer = nullptr)
        : runner_{[this, &pipeline, &executor, observer] {
            result_ = pipeline.run(executor, stop_.get_token(), observer);
            // A rejected concurrent run does not own the other run's workers.
            if (result_ || result_.error() != PipelineError::kBusy)
                pipeline.joinOrphans();
            complete_.store(true, std::memory_order_release);
        }} {}
    ~RunScope() { request_stop(); (void)join(); }
    RunScope(const RunScope&) = delete;
    RunScope& operator=(const RunScope&) = delete;

    /**
     * Requests that the run stop, as if its stop token had been signalled.
     * @return true if this call made the request; false if stop was already
     *         requested.
     * @note Thread-safe. Mirrors std::stop_source::request_stop().
     */
    bool request_stop() noexcept { return stop_.request_stop(); }

    /**
     * Reports whether the run thread has finished its run and joined orphans.
     * @return true once the run is complete.
     * @note Thread-safe.
     */
    [[nodiscard]] bool complete() const noexcept
    {
        return complete_.load(std::memory_order_acquire);
    }

    /**
     * Waits for the run thread to finish and returns the run's result.
     * @return Empty on success, or the error the run ended with. Repeat calls
     *         return the same result.
     * @note Calls are serialized. Do not call from a job, a stop callback or a
     *       thread the executor needs.
     */
    [[nodiscard]] std::expected<void, PipelineError> join()
    {
        std::lock_guard lock{joinMutex_};
        if (runner_.joinable()) runner_.join();
        return result_;
    }
private:
    std::stop_source stop_;
    std::atomic<bool> complete_{false};
    std::expected<void, PipelineError> result_;
    std::mutex joinMutex_;
    std::jthread runner_;
};

} // namespace sub0pipeline
