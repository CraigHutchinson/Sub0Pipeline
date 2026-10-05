// platform/desktop/desktop_executor.cpp
//
// std::thread-based executor for Sub0Pipeline on desktop platforms.
// Each dispatched job runs as a joinable std::thread.
// Used for desktop simulation and integration testing with real parallelism.

#include <sub0pipeline/executor.hpp>
#include <sub0pipeline/executor_factory.hpp>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sub0pipeline {

class DesktopExecutor final : public IExecutor
{
public:
    /// Joins every dispatched job; a std::thread must not die joinable.
    ~DesktopExecutor() override { wait_all(); }

    void dispatch(
        std::string_view              /*name*/,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           /*coreAffinity*/,
        uint8_t                       /*priority*/,
        uint32_t                      /*stackBytes*/) override
    {
        std::lock_guard lk{mtx_};
        ++inFlight_;
        threads_.emplace_back([this, fn = std::move(fn), oc = std::move(onComplete)]
        {
            fn();
            if (oc) oc();
            // Publish completion under the lock so a waiter cannot miss it.
            std::lock_guard done{mtx_};
            if (--inFlight_ == 0U) idle_.notify_all();
        });
    }

    void wait_all() override
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

    [[nodiscard]] int concurrency() const noexcept override
    {
        return static_cast<int>(std::thread::hardware_concurrency());
    }

private:
    std::mutex                mtx_;      ///< Guards threads_ and inFlight_.
    std::condition_variable   idle_;     ///< Signalled when inFlight_ reaches zero.
    std::vector<std::thread>  threads_;  ///< Started since the last wait_all() join.
    uint32_t                  inFlight_{0U};
};

/** @return A DesktopExecutor backed by std::thread (one thread per job). */
std::unique_ptr<IExecutor> makeDesktopExecutor()
{
    return std::make_unique<DesktopExecutor>();
}

} // namespace sub0pipeline
