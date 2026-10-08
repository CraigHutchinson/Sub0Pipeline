#pragma once
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <QRunnable>
#include <QThreadPool>

#include "sub0pipeline/sub0pipeline.hpp"

/** Runs tasks on a private Qt pool with caller-runs overflow.
 *
 * Jobs must be affinity-independent and tolerate recursive inline successors.
 * Bodies, completion callbacks and callable destructors must not throw.
 * Do not call waitAll from a job. The owner must outlive all dispatch producers
 * and joined work. No GUI event loop or GUI completion callback is required.
 */
class QtExecutor final : public sub0pipeline::IExecutor
{
public:
    /** Creates a private pool. @param workers Maximum workers, clamped to at least one. */
    explicit QtExecutor(int workers = 2) { pool_.setMaxThreadCount(std::max(1, workers)); }
    ~QtExecutor() override { waitAll(); pool_.waitForDone(); }
    void dispatch(std::string_view, std::function<void()> fn,
                  std::function<void()> complete, int, uint8_t, uint32_t) override
    {
        auto work = [this, fn = std::move(fn), complete = std::move(complete)]() mutable
        {
            fn();
            if (complete) complete();
            // Destroy borrowed callable captures before publishing completion.
            complete = {}; fn = {};
            std::lock_guard lock{mutex_};
            --pending_;
            ready_.notify_all();
        };
        std::unique_ptr<QRunnable> runnable{QRunnable::create(std::move(work))};
        {
            std::lock_guard lock{mutex_};
            ++pending_;
        }
        bool started;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        try
        {
#endif
            started = pool_.tryStart(runnable.get());
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        }
        catch (...)
        {
            std::lock_guard lock{mutex_};
            --pending_;
            ready_.notify_all();
            throw;
        }
#endif
        if (started)
        {
            (void)runnable.release();
        }
        else
        {
            runnable->run();
        }
    }
    void waitAll() override
    {
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
