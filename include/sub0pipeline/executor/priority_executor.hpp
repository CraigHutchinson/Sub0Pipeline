// include/sub0pipeline/executor/priority_executor.hpp
//
// PriorityExecutor — bounded worker pool that starts higher-priority jobs first.
// Link Sub0Pipeline::Priority.
#pragma once

#include <sub0pipeline/executor/executor.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <vector>

namespace sub0pipeline {

/**
 * @brief Fixed-size worker pool that starts queued jobs in priority order.
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
 * nor movable. dispatch() is thread-safe. Do not call wait_all() from inside a
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
    };

    /** Start a pool with default Options. */
    PriorityExecutor();

    /** Start a pool configured by @p options. */
    explicit PriorityExecutor(Options options);

    /** Waits for every dispatched job, then stops and joins the workers. */
    ~PriorityExecutor() override;

    PriorityExecutor(const PriorityExecutor&)            = delete;
    PriorityExecutor& operator=(const PriorityExecutor&) = delete;

    void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           coreAffinity,
        uint8_t                       priority,
        uint32_t                      stackBytes) override;

    void wait_all() override;

    /** @return The number of worker threads. */
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

    std::priority_queue<QueuedJob>  queue_;             ///< Guarded by mtx_.
    std::mutex                      mtx_;
    std::condition_variable         wake_;              ///< Work queued, or stopping.
    unsigned int                    idle_{0U};          ///< Workers waiting on wake_; guarded by mtx_.
    bool                            stopping_{false};   ///< Guarded by mtx_.
    std::vector<std::jthread>       workers_;
    std::atomic<uint32_t>           inFlight_{0U};
    std::mutex                      doneMtx_;
    std::condition_variable         doneCv_;
};

/**
 * @brief Heap-allocate a PriorityExecutor behind the IExecutor interface.
 *
 * @param threadCount    See PriorityExecutor::Options::threadCount.
 * @param onThreadStart  See PriorityExecutor::Options::onThreadStart.
 * @return An owning pointer.
 */
std::unique_ptr<IExecutor> makePriorityExecutor(
    unsigned int threadCount = 0, std::function<void()> onThreadStart = nullptr);

} // namespace sub0pipeline
