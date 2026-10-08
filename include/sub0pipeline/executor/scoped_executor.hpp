// include/sub0pipeline/executor/scoped_executor.hpp
//
// ScopedExecutor — scopes waitAll() to its own dispatches for nested sub-DAG runs.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "sub0pipeline/executor/executor.hpp"

namespace sub0pipeline
{

// ── ScopedExecutor ───────────────────────────────────────────────────────────

/**
 * Scopes waitAll() to the jobs dispatched through this instance, so a
 * running job can execute a sub-DAG.
 *
 * The canonical deadlock scenario without ScopedExecutor:
 *   - Outer job T runs on DesktopExecutor (inFlight_ counts T).
 *   - T calls inner.run(outerExec) -> inner.run() calls outerExec.waitAll().
 *   - outerExec.waitAll() waits for inFlight_ == 0, but T contributes 1
 *     and T is the waiter -> deadlock.
 *
 * ScopedExecutor fixes this by maintaining its own inFlight_ counter.
 * waitAll() waits only for jobs dispatched through this scope -- not for
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
 *
 * A parent submission exception rejects both callbacks and is rethrown after
 * local accounting is restored. Accepted bodies and callbacks must not throw.
 * Packet and wrapper storage can allocate before the parent sees a submission.
 * Body-owned state remains alive through completion; both original targets are
 * destroyed before this scope publishes completion, even if the parent retains
 * the wrapper functions.
 */
class ScopedExecutor final : public IExecutor
{
public:
    /**
     * Creates a scope over @p parent.
     * @param parent  The executor that actually runs the jobs. Borrowed; it
     *                must outlive this scope.
     */
    explicit ScopedExecutor(IExecutor& parent) noexcept : parent_{parent} {}
    ~ScopedExecutor() override { waitAll(); }

    void dispatch(
        std::string_view      name,
        std::function<void()> fn,
        std::function<void()> onComplete,
        int                   coreAffinity,
        uint8_t               priority,
        uint32_t              stackBytes) override
    {
        auto state = std::make_shared<DispatchState>(DispatchState{std::move(fn), std::move(onComplete)});
        std::function<void()> body = [state] { state->fn_(); };
        std::function<void()> completion = [this, state]
        {
            if (state->onComplete_) state->onComplete_();
            state->clearTargets();
            finishDispatch();
        };
        localInFlight_.fetch_add(1U, std::memory_order_relaxed);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        try
        {
#endif
            parent_.dispatch(name, std::move(body), std::move(completion),
                             coreAffinity, priority, stackBytes);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        }
        catch (...)
        {
            state->clearTargets();
            finishDispatch();
            throw;
        }
#endif
    }

    void waitAll() override
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

    [[nodiscard]] bool runsInline() const noexcept override
    {
        return parent_.runsInline();
    }

private:
    struct DispatchState
    {
        std::function<void()> fn_;
        std::function<void()> onComplete_;

        void clearTargets() noexcept
        {
            onComplete_ = {};
            fn_ = {};
        }
    };

    void finishDispatch()
    {
        // Publish completion and notify before the waiter can destroy us.
        std::lock_guard lock{mtx_};
        if (localInFlight_.fetch_sub(1U, std::memory_order_acq_rel) == 1U)
            cv_.notify_all();
    }

    IExecutor&                parent_;
    std::atomic<uint32_t>     localInFlight_{0U};
    std::mutex                mtx_;
    std::condition_variable   cv_;
};

} // namespace sub0pipeline
