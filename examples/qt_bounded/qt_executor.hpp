#pragma once
#include <sub0pipeline/sub0pipeline.hpp>
#include <QThreadPool>
#include <algorithm>
#include <condition_variable>

// No Qt queue: when all workers are busy, execute on the dispatching thread.
// This avoids worker-side enqueue deadlocks. Jobs must be affinity-independent,
// non-throwing, and tolerate recursive inline successors. Never call wait_all
// from a job. No GUI event loop is required and no GUI callbacks are awaited.
class QtExecutor final : public sub0pipeline::IExecutor {
public:
    explicit QtExecutor(int workers = 2) { pool_.setMaxThreadCount(std::max(1, workers)); }
    ~QtExecutor() override { wait_all(); pool_.waitForDone(); }
    void dispatch(std::string_view, std::function<void()> fn,
                  std::function<void()> complete, int, uint8_t, uint32_t) override {
        {
            std::lock_guard lock{mutex_};
            ++pending_;
        }
        auto work = [this, fn = std::move(fn), complete = std::move(complete)]() mutable {
            fn();
            if (complete) complete();
            // Destroy borrowed callable captures before publishing completion.
            fn = {}; complete = {};
            std::lock_guard lock{mutex_};
            --pending_;
            ready_.notify_all();
        };
        // Copy, because a failed tryStart may consume an rvalue callable.
        if (!pool_.tryStart(work)) work();
    }
    void wait_all() override {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [&] { return pending_ == 0; });
    }
    int concurrency() const noexcept override { return pool_.maxThreadCount(); }
private:
    QThreadPool pool_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::size_t pending_ = 0;
};
