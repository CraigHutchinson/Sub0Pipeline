// platform/priority/priority_executor.cpp
//
// Bounded thread-pool executor that honours job priority.
// Higher-priority jobs (larger uint8_t value) preempt lower-priority ones
// that have not yet started executing.
//
// Primary use case: UDAW distinguishes "blocking" fetches (Editor is waiting,
// priority 10) from "prefetch" hints (background, priority 5). The pool runs
// both but always starts the blocking fetch first.

#include <sub0pipeline/sub0pipeline.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace sub0pipeline {

namespace {
    /// 0-byte placeholder for the enqueue-sequence field in the lean (unstable)
    /// executor, so a memory-constrained target carries no per-job ordering cost.
    struct NoSeq {};
}

/**
 * Bounded thread-pool executor honouring job priority.
 *
 * @tparam Stable  When true, equal-priority jobs run in dispatch order (FIFO)
 *                 via a per-job enqueue sequence -- the guarantee callers that
 *                 dispatch an ordered sequence (e.g. an access-order prefetch)
 *                 or need peer fairness depend on. When false, equal-priority
 *                 order is the heap's arbitrary order and no per-job sequence is
 *                 stored, so a memory-constrained target that does not need FIFO
 *                 among peers pays nothing for the ordering. `makePriorityExecutor`
 *                 selects the stable policy; `makeLeanPriorityExecutor` the lean one.
 */
template<bool Stable>
class PriorityExecutorT final : public IExecutor
{
    struct Job
    {
        std::function<void()> fn;
        std::function<void()> onComplete;
        uint8_t               priority{5};
        // Present only in the stable policy; NoSeq is empty so the lean job pays
        // no per-job cost. std::priority_queue is a max-heap on operator<, so the
        // "greatest" Job pops first: higher priority is greater, and within one
        // priority the LOWER seq (enqueued earlier) compares greater so it pops
        // first. Without the tie-break a later-dispatched job can overtake an
        // earlier one, which is unacceptable where dispatch order carries meaning.
        [[no_unique_address]] std::conditional_t<Stable, uint64_t, NoSeq> seq{};

        bool operator<(const Job& o) const noexcept
        {
            if (priority != o.priority) return priority < o.priority;
            if constexpr (Stable) return seq > o.seq;
            else                  return false;   // equal priority: heap order (lean)
        }
    };

public:
    PriorityExecutorT(unsigned int threadCount, std::function<void()> onThreadStart)
    {
        workers_.reserve(threadCount);
        for (unsigned int i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this, onThreadStart](std::stop_token st) {
                if (onThreadStart) onThreadStart();
                while (!st.stop_requested()) {
                    Job job;
                    {
                        std::unique_lock lk{mtx_};
                        cv_.wait(lk, st, [this]{ return !queue_.empty(); });
                        if (queue_.empty()) break; // stop requested
                        job = std::move(const_cast<Job&>(queue_.top()));
                        queue_.pop();
                    }
                    job.fn();
                    if (job.onComplete) job.onComplete();
                    if (inFlight_.fetch_sub(1U, std::memory_order_acq_rel) == 1U)
                        doneCv_.notify_all();
                }
            });
        }
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
        {
            std::lock_guard lk{mtx_};
            if constexpr (Stable)
                queue_.push(Job{std::move(fn), std::move(onComplete), priority, seq_++});
            else
                queue_.push(Job{std::move(fn), std::move(onComplete), priority});
        }
        cv_.notify_one();
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
    std::priority_queue<Job>        queue_;
    // Monotonic enqueue counter, guarded by mtx_, stamps Job::seq -- stable policy
    // only (NoSeq is empty, so the lean executor object carries no counter either).
    [[no_unique_address]] std::conditional_t<Stable, uint64_t, NoSeq> seq_{};
    std::mutex                      mtx_;
    std::condition_variable_any     cv_;
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
    return std::make_unique<PriorityExecutorT<true>>(threadCount, std::move(onThreadStart));
}

std::unique_ptr<IExecutor> makeLeanPriorityExecutor(unsigned int threadCount,
                                                     std::function<void()> onThreadStart)
{
    if (threadCount == 0)
        threadCount = std::max(1U, std::thread::hardware_concurrency());
    return std::make_unique<PriorityExecutorT<false>>(threadCount, std::move(onThreadStart));
}

} // namespace sub0pipeline
