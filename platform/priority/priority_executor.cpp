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
#include <vector>

namespace sub0pipeline {

class PriorityExecutor final : public IExecutor
{
    struct Job
    {
        std::function<void()> fn;
        std::function<void()> onComplete;
        uint8_t               priority{5};

        bool operator<(const Job& o) const noexcept { return priority < o.priority; }
    };

public:
    explicit PriorityExecutor(unsigned int threadCount)
    {
        workers_.reserve(threadCount);
        for (unsigned int i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this](std::stop_token st) {
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
    std::mutex                      mtx_;
    std::condition_variable_any     cv_;
    std::vector<std::jthread>       workers_;
    std::atomic<uint32_t>           inFlight_{0U};
    std::mutex                      doneMtx_;
    std::condition_variable         doneCv_;
};

std::unique_ptr<IExecutor> makePriorityExecutor(unsigned int threadCount)
{
    if (threadCount == 0)
        threadCount = std::max(1U, std::thread::hardware_concurrency());
    return std::make_unique<PriorityExecutor>(threadCount);
}

} // namespace sub0pipeline
