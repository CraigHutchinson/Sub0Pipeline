// platform/desktop/desktop_executor.cpp
//
// std::thread-based executor for Sub0Pipeline on desktop platforms.
// Each dispatched job runs as a joinable std::thread.
// Used for desktop simulation and integration testing with real parallelism.

#include <sub0pipeline/sub0pipeline.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sub0pipeline {

class DesktopExecutor final : public IExecutor
{
public:
    void dispatch(
        std::string_view              /*name*/,
        std::function<void()>         fn,
        std::function<void()>         on_complete,
        int                           /*core*/,
        uint8_t                       /*priority*/,
        uint32_t                      /*stack_bytes*/) override
    {
        inFlight_.fetch_add(1U, std::memory_order_relaxed);
        std::lock_guard lk{mtx_};
        threads_.emplace_back([this, fn = std::move(fn), oc = std::move(on_complete)]
        {
            fn();
            if (oc) oc();
            if (inFlight_.fetch_sub(1U, std::memory_order_release) == 1U) {
                cv_.notify_all();
            }
        });
    }

    void wait_all() override
    {
        // Drain loop: successor jobs may be dispatched during execution, so we
        // keep joining until no threads remain and in_flight_ reaches zero.
        while (true) {
            std::vector<std::thread> batch;
            {
                std::lock_guard lk{mtx_};
                if (threads_.empty() && inFlight_.load(std::memory_order_acquire) == 0U) break;
                batch = std::move(threads_);
            }
            for (auto& t : batch) {
                if (t.joinable()) t.join();
            }
            // If no threads were moved but in_flight > 0, wait briefly for new dispatches.
            if (batch.empty()) {
                std::unique_lock lk{mtx_};
                cv_.wait_for(lk, std::chrono::milliseconds{10}, [this]
                {
                    return !threads_.empty()
                        || inFlight_.load(std::memory_order_acquire) == 0U;
                });
            }
        }
    }

    [[nodiscard]] int concurrency() const noexcept override
    {
        return static_cast<int>(std::thread::hardware_concurrency());
    }

private:
    std::mutex                mtx_;
    std::condition_variable   cv_;
    std::vector<std::thread>  threads_;
    std::atomic<uint32_t>     inFlight_{0U};
};

/** @return A DesktopExecutor backed by std::thread (one thread per job). */
std::unique_ptr<IExecutor> make_desktop_executor()
{
    return std::make_unique<DesktopExecutor>();
}

} // namespace sub0pipeline
