// include/sub0pipeline/executor/priority_executor.hpp
//
// PriorityExecutor — bounded worker pool that starts higher-priority jobs first.
// Link Sub0Pipeline::Priority.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <vector>

#include "sub0pipeline/executor/executor.hpp"

namespace sub0pipeline
{

/**
 * Runs jobs on a fixed-size worker pool, starting queued jobs in priority order.
 *
 * A job with a larger `.priority()` value starts before lower-priority jobs
 * that are still queued. Running jobs are not preempted, and jobs of equal
 * priority start in no particular order. Core affinity and stack hints are
 * ignored.
 *
 * Typical use: work a client is waiting on (`.priority(10)`) overtakes
 * background prefetching (`.priority(5)`) when the pool is busy.
 *
 * Owns its worker threads, which start in the constructor. The destructor
 * waits for every dispatched job and then joins the workers. Construct it
 * wherever suits the caller (stack, member, static); it is neither copyable
 * nor movable. dispatch() is thread-safe. Do not call waitAll() from inside a
 * job running on this executor; use ScopedExecutor for nested runs.
 */
class PriorityExecutor final : public IExecutor
{
public:
    /** Construction settings. Every field has a usable default. */
    struct Options
    {
        /** Worker thread count. 0 selects `std::thread::hardware_concurrency()`, at least 1. */
        unsigned int threadCount{0U};

        /**
         * Called once on each worker thread before it runs any job, on that
         * thread. Use it for per-thread setup that would otherwise land on the
         * first job: naming the thread, pinning affinity, or touching
         * thread-local state. Must not throw. Empty for none.
         */
        std::function<void()> onThreadStart{};

        /**
         * Maximum queued jobs, excluding jobs already running. 0 preserves
         * the dynamically growing queue. A positive value reserves queue
         * storage before workers start; dispatch rejects a full queue.
         * Must fit uint32_t after adding the worker count.
         */
        std::size_t queueCapacity{0U};
    };

    /** Start a pool with default Options. */
    PriorityExecutor();

    /**
     * Start a pool configured by @p options.
     * @param options  Worker count, per-thread setup and optional queue bound.
     * @throws std::runtime_error Invalid worker/count bounds with exceptions enabled.
     * @note Startup allocation or thread creation can fail. Already-started
     *       workers are stopped and joined before an exception propagates.
     */
    explicit PriorityExecutor(Options options);

    /** Waits for every dispatched job, then stops and joins the workers. */
    ~PriorityExecutor() override;

    PriorityExecutor(const PriorityExecutor&)            = delete;
    PriorityExecutor& operator=(const PriorityExecutor&) = delete;

    /**
     * Accepts a job, or rejects it without retaining or invoking either callback.
     * @note A full bounded queue reports a hard error according to
     *       SUB0PIPELINE_EXCEPTIONS. Other submission exceptions propagate.
     *       Accepted bodies and completion callbacks must not throw.
     *       Callable storage may allocate before entry, even with a queue bound.
     */
    void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           coreAffinity,
        uint8_t                       priority,
        uint32_t                      stackBytes) override;

    void waitAll() override;

    /** Returns the number of worker threads. */
    [[nodiscard]] int concurrency() const noexcept override;

private:
    struct QueuedJob
    {
        std::function<void()> fn;
        std::function<void()> onComplete;
        uint8_t               priority{5};

        bool operator<(const QueuedJob& o) const noexcept { return priority < o.priority; }
    };

    void work(const std::function<void()>& onThreadStart);
    void stopWorkers() noexcept;

    std::priority_queue<QueuedJob>  queue_;             ///< Guarded by mtx_.
    std::size_t                     queueCapacity_{0U};
    std::mutex                      mtx_;
    std::condition_variable         wake_;              ///< Work queued, or stopping.
    unsigned int                    idle_{0U};          ///< Workers waiting on wake_; guarded by mtx_.
    bool                            stopping_{false};   ///< Guarded by mtx_.
    std::vector<std::jthread>       workers_;
    std::atomic<uint32_t>           inFlight_{0U};
    std::mutex                      doneMtx_;
    std::condition_variable         doneCv_;
};

} // namespace sub0pipeline
