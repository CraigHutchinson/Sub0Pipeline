// platform/desktop/desktop_executor.cpp
//
// std::thread-based executor for Sub0Pipeline on desktop platforms.
// Each dispatched job runs as a joinable std::thread.
// Used for desktop simulation and integration testing with real parallelism.

#include "sub0pipeline/executor/desktop_executor.hpp"

#include <utility>

namespace sub0pipeline {

// A std::thread must not die joinable.
DesktopExecutor::~DesktopExecutor() { wait_all(); }

void DesktopExecutor::dispatch(
    std::string_view              /*name*/,
    std::function<void()>         fn,
    std::function<void()>         onComplete,
    int                           /*coreAffinity*/,
    uint8_t                       /*priority*/,
    uint32_t                      /*stackBytes*/)
{
    std::lock_guard lk{mtx_};
    threads_.emplace_back([this, fn = std::move(fn), oc = std::move(onComplete)]
    {
        fn();
        if (oc) oc();
        // Publish completion under the lock so a waiter cannot miss it.
        std::lock_guard done{mtx_};
        if (--inFlight_ == 0U) idle_.notify_all();
    });
    // Counted only once the thread exists: if starting it throws, nothing
    // is left in flight. The new thread cannot decrement before this,
    // because it needs mtx_ to do so.
    ++inFlight_;
}

void DesktopExecutor::wait_all()
{
    std::unique_lock lk{mtx_};
    // Jobs may dispatch successors, and joining releases the lock, so
    // repeat until a pass finds nothing running and nothing left to join.
    while (inFlight_ != 0U || !threads_.empty()) {
        idle_.wait(lk, [this] { return inFlight_ == 0U; });
        auto finished = std::move(threads_);
        threads_.clear();
        lk.unlock();
        for (auto& thread : finished) thread.join();
        lk.lock();
    }
}

int DesktopExecutor::concurrency() const noexcept
{
    return static_cast<int>(std::thread::hardware_concurrency());
}

} // namespace sub0pipeline
