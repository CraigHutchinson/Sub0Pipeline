// platform/priority/priority_executor.cpp
//
// Bounded thread-pool executor that honours job priority.
// Higher-priority jobs (larger uint8_t value) preempt lower-priority ones
// that have not yet started executing.
//
// Primary use case: A device distinguishes "blocking" fetches (a client is waiting,
// priority 10) from "prefetch" hints (background, priority 5). The pool runs
// both but always starts the blocking fetch first.

#include <sub0pipeline/executor/priority_executor.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sub0pipeline {

class PriorityExecutor final : public IExecutor
{
    struct QueuedJob
    {
        std::function<void()> fn;
        std::function<void()> onComplete;
        uint8_t               priority{5};

        bool operator<(const QueuedJob& o) const noexcept { return priority < o.priority; }
    };

public:
    PriorityExecutor(unsigned int threadCount, std::function<void()> onThreadStart)
    {
        workers_.reserve(threadCount);
        for (unsigned int i = 0; i < threadCount; ++i)
            workers_.emplace_back([this, onThreadStart] { work(onThreadStart); });
    }

    ~PriorityExecutor() override
    {
        wait_all();
        {
            std::lock_guard lk{mtx_};
            stopping_ = true;
        }
        wake_.notify_all();
        // Join while all synchronization members are still alive.
        workers_.clear();
    }

    void dispatch(
        std::string_view              /*name*/,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           /*coreAffinity*/,
        uint8_t                       priority,
        uint32_t                      /*stackBytes*/) override
    {
        inFlight_.fetch_add(1U, std::memory_order_relaxed);
        bool wake;
        {
            std::lock_guard lk{mtx_};
            queue_.push(QueuedJob{std::move(fn), std::move(onComplete), priority});
            // A busy worker re-checks the queue before it sleeps, so a wake-up
            // is only needed when some worker is already asleep.
            wake = idle_ != 0U;
        }
        if (wake) wake_.notify_one();
    }

    void wait_all() override
    {
        std::unique_lock lk{doneMtx_};
        doneCv_.wait(lk, [this]{ return inFlight_.load(std::memory_order_acquire) == 0U; });
    }

    [[nodiscard]] int concurrency() const noexcept override
    {
        return static_cast<int>(workers_.size());
    }

private:
    void work(const std::function<void()>& onThreadStart)
    {
        if (onThreadStart) onThreadStart();
        for (;;) {
            QueuedJob job;
            {
                std::unique_lock lk{mtx_};
                ++idle_;
                wake_.wait(lk, [this]{ return stopping_ || !queue_.empty(); });
                --idle_;
                if (queue_.empty()) return; // stopping, and nothing left to drain
                job = std::move(const_cast<QueuedJob&>(queue_.top()));
                queue_.pop();
            }
            job.fn();
            if (job.onComplete) job.onComplete();
            // Only the completion that empties the executor has a waiter to
            // wake. Taking doneMtx_ there orders the notify after the waiter's
            // predicate check, so it cannot be missed.
            if (inFlight_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
                std::lock_guard done{doneMtx_};
                doneCv_.notify_all();
            }
        }
    }

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

std::unique_ptr<IExecutor> makePriorityExecutor(unsigned int threadCount,
                                                 std::function<void()> onThreadStart)
{
    if (threadCount == 0)
        threadCount = std::max(1U, std::thread::hardware_concurrency());
    return std::make_unique<PriorityExecutor>(threadCount, std::move(onThreadStart));
}

} // namespace sub0pipeline
