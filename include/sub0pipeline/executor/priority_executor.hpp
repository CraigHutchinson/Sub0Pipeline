// include/sub0pipeline/executor/priority_executor.hpp
//
// PriorityExecutor / StablePriorityExecutor — bounded worker pool that starts
// higher-priority jobs first.
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

namespace detail
{
    /** Dispatch sequence of a queued job; empty unless the executor keeps dispatch order. */
    template<bool Stable>
    struct PriorityJobSequence {};

    template<>
    struct PriorityJobSequence<true>
    {
        uint64_t sequence{0U};
    };
} // namespace detail

/**
 * Runs jobs on a fixed-size worker pool, starting queued jobs in priority order.
 *
 * A job with a larger `.priority()` value starts before lower-priority jobs
 * that are still queued. Running jobs are not preempted. Core affinity and
 * stack hints are ignored.
 *
 * Typical use: work a client is waiting on (`.priority(10)`) overtakes
 * background prefetching (`.priority(5)`) when the pool is busy.
 *
 * Owns its worker threads, which start in the constructor. The destructor
 * waits for every dispatched job and then joins the workers. Construct it
 * wherever suits the caller (stack, member, static); it is neither copyable
 * nor movable. dispatch() is thread-safe. Do not call waitAll() from inside a
 * job running on this executor; use ScopedExecutor for nested runs.
 *
 * @tparam Stable  Whether equal-priority jobs start in dispatch order.
 *                 PriorityExecutor (false) starts them in no particular order
 *                 and stores nothing extra per job. StablePriorityExecutor
 *                 (true) starts them in dispatch order, for callers whose
 *                 dispatch order carries meaning, such as an access-order
 *                 prefetch.
 */
template<bool Stable>
class BasicPriorityExecutor final : public IExecutor
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
    BasicPriorityExecutor();

    /**
     * Start a pool configured by @p options.
     * @param options  Worker count, per-thread setup and optional queue bound.
     * @throws std::runtime_error Invalid worker/count bounds with exceptions enabled.
     * @note Startup allocation or thread creation can fail. Already-started
     *       workers are stopped and joined before an exception propagates.
     */
    explicit BasicPriorityExecutor(Options options);

    /** Waits for every dispatched job, then stops and joins the workers. */
    ~BasicPriorityExecutor() override;

    BasicPriorityExecutor(const BasicPriorityExecutor&)            = delete;
    BasicPriorityExecutor& operator=(const BasicPriorityExecutor&) = delete;

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
    // An empty base, not a member, so PriorityExecutor's queued job stays the same size on MSVC,
    // which ignores [[no_unique_address]].
    struct QueuedJob : detail::PriorityJobSequence<Stable>
    {
        std::function<void()> fn;
        std::function<void()> onComplete;
        uint8_t               priority{5};

        /** priority_queue pops the greatest job: the higher priority, then the earlier dispatch. */
        bool operator<(const QueuedJob& o) const noexcept
        {
            if (priority != o.priority) return priority < o.priority;
            if constexpr (Stable) return this->sequence > o.sequence;
            else return false;
        }
    };

    void work(const std::function<void()>& onThreadStart);
    void stopWorkers() noexcept;

    std::priority_queue<QueuedJob>  queue_;             ///< Guarded by mtx_.
    uint64_t                        nextSequence_{0U};  ///< Guarded by mtx_; read only when Stable.
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

using PriorityExecutor       = BasicPriorityExecutor<false>;
using StablePriorityExecutor = BasicPriorityExecutor<true>;

extern template class BasicPriorityExecutor<false>;
extern template class BasicPriorityExecutor<true>;

} // namespace sub0pipeline
