// platform/priority/priority_executor.cpp
//
// Bounded thread-pool executor that honours job priority.
// Higher-priority jobs (larger uint8_t value) start before lower-priority ones
// that have not yet started executing.

#include "sub0pipeline/executor/priority_executor.hpp"

#include <algorithm>
#include <utility>

namespace sub0pipeline
{

PriorityExecutor::PriorityExecutor() : PriorityExecutor{Options{}} {}

PriorityExecutor::PriorityExecutor(Options options)
{
    const unsigned int threadCount = options.threadCount != 0U
        ? options.threadCount
        : std::max(1U, std::thread::hardware_concurrency());
    workers_.reserve(threadCount);
    for (unsigned int i = 0; i < threadCount; ++i)
    {
        workers_.emplace_back([this, onThreadStart = options.onThreadStart] {
            work(onThreadStart);
        });
    }
}

PriorityExecutor::~PriorityExecutor()
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

void PriorityExecutor::dispatch(
    std::string_view              /*name*/,
    std::function<void()>         fn,
    std::function<void()>         onComplete,
    int                           /*coreAffinity*/,
    uint8_t                       priority,
    uint32_t                      /*stackBytes*/)
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

void PriorityExecutor::wait_all()
{
    std::unique_lock lk{doneMtx_};
    doneCv_.wait(lk, [this]{ return inFlight_.load(std::memory_order_acquire) == 0U; });
}

int PriorityExecutor::concurrency() const noexcept
{
    return static_cast<int>(workers_.size());
}

void PriorityExecutor::work(const std::function<void()>& onThreadStart)
{
    if (onThreadStart) onThreadStart();
    for (;;)
    {
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
        if (inFlight_.fetch_sub(1U, std::memory_order_acq_rel) == 1U)
        {
            std::lock_guard done{doneMtx_};
            doneCv_.notify_all();
        }
    }
}

} // namespace sub0pipeline
