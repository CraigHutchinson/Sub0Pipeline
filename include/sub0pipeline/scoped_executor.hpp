// include/sub0pipeline/scoped_executor.hpp
//
// ScopedExecutor — scopes wait_all() to its own dispatches for nested sub-DAG runs.
#pragma once

#include <sub0pipeline/executor.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <utility>

namespace sub0pipeline {

// ── ScopedExecutor ───────────────────────────────────────────────────────────

/**
 * @brief Executor wrapper that scopes wait_all() to jobs dispatched through
 *        this instance, enabling sub-DAG execution from within a running job.
 *
 * The canonical deadlock scenario without ScopedExecutor:
 *   - Outer job T runs on DesktopExecutor (inFlight_ counts T).
 *   - T calls inner.run(outerExec) -> inner.run() calls outerExec.wait_all().
 *   - outerExec.wait_all() waits for inFlight_ == 0, but T contributes 1
 *     and T is the waiter -> deadlock.
 *
 * ScopedExecutor fixes this by maintaining its own inFlight_ counter.
 * wait_all() waits only for jobs dispatched through this scope -- not for
 * the caller's own contribution to the parent executor's inFlight_.
 *
 * The parent executor's thread pool is reused (no extra threads created):
 * @code
 *   outer.emplace([&exec]() -> std::expected<void, PipelineError> {
 *       ScopedExecutor scoped{exec};
 *       Pipeline inner;
 *       inner >> "a"_job(fn_a) >> "b"_job(fn_b);  // shape known at runtime
 *       return inner.run(scoped);
 *   }).name("dynamic_step");
 *   outer.run(exec);
 * @endcode
 */
class ScopedExecutor final : public IExecutor
{
public:
    explicit ScopedExecutor(IExecutor& parent) noexcept : parent_{parent} {}
    ~ScopedExecutor() override { wait_all(); }

    void dispatch(
        std::string_view      name,
        std::function<void()> fn,
        std::function<void()> onComplete,
        int                   coreAffinity,
        uint8_t               priority,
        uint32_t              stackBytes = 4096U) override
    {
        localInFlight_.fetch_add(1U, std::memory_order_relaxed);
        parent_.dispatch(
            name,
            std::move(fn),
            [this, oc = std::move(onComplete)]() mutable
            {
                if (oc) oc();
                // Publish completion and notify before the waiter can destroy us.
                std::lock_guard lock{mtx_};
                if (localInFlight_.fetch_sub(1U, std::memory_order_acq_rel) == 1U)
                    cv_.notify_all();
            },
            coreAffinity, priority, stackBytes);
    }

    void wait_all() override
    {
        std::unique_lock lk{mtx_};
        cv_.wait(lk, [this]
        {
            return localInFlight_.load(std::memory_order_acquire) == 0U;
        });
    }

    [[nodiscard]] int concurrency() const noexcept override
    {
        return parent_.concurrency();
    }

    [[nodiscard]] bool runs_inline() const noexcept override
    {
        return parent_.runs_inline();
    }

private:
    IExecutor&                parent_;
    std::atomic<uint32_t>     localInFlight_{0U};
    std::mutex                mtx_;
    std::condition_variable   cv_;
};

} // namespace sub0pipeline
