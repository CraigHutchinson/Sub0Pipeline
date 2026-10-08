// platform/priority/priority_executor.cpp
//
// Bounded thread-pool executor that honours job priority.
// Higher-priority jobs (larger uint8_t value) start before lower-priority ones
// that have not yet started executing.

#include <algorithm>
#include <limits>
#include <utility>

#include "sub0pipeline/config.hpp"
#include "sub0pipeline/executor/priority_executor.hpp"

namespace sub0pipeline
{

PriorityExecutor::PriorityExecutor() : PriorityExecutor{Options{}} {}

PriorityExecutor::PriorityExecutor(Options options) : queueCapacity_{options.queueCapacity}
{
    const unsigned int threadCount = options.threadCount != 0U
        ? options.threadCount
        : std::max(1U, std::thread::hardware_concurrency());
    if (threadCount > static_cast<unsigned int>(std::numeric_limits<int>::max()) ||
        queueCapacity_ > std::numeric_limits<uint32_t>::max() - threadCount)
    {
        SUB0PIPELINE_THROW("PriorityExecutor worker/queue counts exceed the supported range");
    }
    if (queueCapacity_ != 0U)
    {
        std::vector<QueuedJob> storage;
        storage.reserve(queueCapacity_);
        queue_ = decltype(queue_){std::less<QueuedJob>{}, std::move(storage)};
    }
    workers_.reserve(threadCount);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        for (unsigned int i = 0; i < threadCount; ++i)
        {
            workers_.emplace_back([this, onThreadStart = options.onThreadStart]
            {
                work(onThreadStart);
            });
        }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (...)
    {
        stopWorkers();
        throw;
    }
#endif
}

PriorityExecutor::~PriorityExecutor()
{
    waitAll();
    stopWorkers();
}

void PriorityExecutor::stopWorkers() noexcept
{
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
    bool wake;
    {
        std::lock_guard lk{mtx_};
        if ((queueCapacity_ != 0U && queue_.size() == queueCapacity_) ||
            inFlight_.load(std::memory_order_relaxed) == std::numeric_limits<uint32_t>::max())
        {
            SUB0PIPELINE_THROW("PriorityExecutor submission capacity exhausted");
        }
        queue_.push(QueuedJob{std::move(fn), std::move(onComplete), priority});
        // Workers cannot remove the accepted job until its count is published.
        inFlight_.fetch_add(1U, std::memory_order_relaxed);
        // A busy worker re-checks the queue before it sleeps, so a wake-up
        // is only needed when some worker is already asleep.
        wake = idle_ != 0U;
    }
    if (wake) wake_.notify_one();
}

void PriorityExecutor::waitAll()
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
        // Completion may borrow state owned only by the body target.
        job.onComplete = {};
        job.fn = {};
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
