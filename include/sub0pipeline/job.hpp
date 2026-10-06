// include/sub0pipeline/job.hpp
//
// Job — builder handle to a DAG node, with its JobId identity and JobStatus.
#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

namespace sub0pipeline {

class JobGroup;
class Pipeline;

/** Stable node index within one Pipeline. Graphs accept at most 65,536 jobs,
 * and one job at most 32,767 successors; exceeding either is a hard error.
 * Append-only node indices remain valid until their owning Pipeline is destroyed.
 */
using JobId = uint32_t;

namespace detail {
/// The part of a Pipeline's heap state that Job handles point at. It does not
/// move when the Pipeline object is moved; `owner` then names the new object.
struct PipelineAnchor
{
    Pipeline* owner{nullptr}; // non-owning; maintained by Pipeline
};
} // namespace detail

// ── Job handle ────────────────────────────────────────────────────────────────

/**
 * Refers to a node in the Pipeline DAG through a lightweight handle.
 *
 * Copyable and comparable. Inspired by Taskflow's tf::Task — a thin wrapper
 * around an internal node index plus a back-pointer to its owning Pipeline.
 * All builder methods return *this for fluent chaining.
 */
class Job
{
public:
    constexpr Job() noexcept = default;

    /** Set a human-readable name (used in tracing and observer callbacks). */
    Job& name(std::string_view n);

    /**
     * Set the maximum execution time for this job.
     *
     * If the job function does not return within `t`, the engine returns
     * `kTimeout` / `kTimedOut` and cascades skip to all successors.
     * The timed-out job remains owned until join_orphans(); functions must
     * also timeout at the syscall level (TCP connect, subprocess pipe).
     *
     * Default: `std::chrono::milliseconds::max()` (no timeout).
     */
    Job& timeout(std::chrono::milliseconds t) noexcept;

    /**
     * Hints that the job should be pinned to a CPU core (-1 = any, the default).
     * @note Honored by FreeRtosExecutor. Ignored by the other bundled executors.
     */
    Job& core(int c) noexcept;

    /**
     * Hints the executor task stack size in bytes (default 8192).
     * @note Honored by FreeRtosExecutor. Ignored by the other bundled executors.
     */
    Job& stack(uint32_t bytes) noexcept;

    /**
     * Hints the executor task priority, 1–24 (default 5).
     * @note Honored by PriorityExecutor (larger starts first) and FreeRtosExecutor
     *       (clamped to 1–24). Ignored by the other bundled executors.
     */
    Job& priority(uint8_t p) noexcept;

    /** Ordinary failure does not block dependents; cancellation remains fatal. */
    Job& optional(bool opt = true) noexcept;

    /**
     * Set display text for this job, such as "Loading settings…".
     *
     * Meant for a progress display: read it back with
     * Pipeline::statusText(JobId) from IObserver::onJobStart, or from a
     * Pipeline::JobSnapshot. It is separate from name(), which identifies the
     * job in logs and traces.
     *
     * @param text  Borrowed, not copied: it must outlive the Pipeline. Pass a
     *              string literal or other static storage. nullptr clears it.
     */
    Job& statusText(const char* text) noexcept;

    /**
     * Request cancellation of this job.
     *
     * Thread-safe. Fires the job's `std::stop_source`, setting its
     * `stop_token` to stopped. Cancellable job functions (those taking
     * `std::stop_token`) check `stop_requested()` and return `kCancelled`.
     * Pending functions are suppressed regardless of their signature. Running
     * non-cooperative functions cannot be interrupted. Pre-run requests reset
     * at the start of the next run, before any job is dispatched.
     */
    void cancel() noexcept;

    /**
     * Declare that this job runs AFTER @p other completes.
     * @param other  The predecessor job.
     * @return *this for chaining.
     * @note May allocate (push_back on predecessor/successor vectors).
     */
    Job& succeed(Job other);

    /**
     * Declare that @p other runs AFTER this job completes.
     * @param other  The successor job.
     * @return *this for chaining.
     * @note May allocate (push_back on predecessor/successor vectors).
     */
    Job& precede(Job other);

    /** Variadic: this job runs after all listed jobs complete. */
    template< typename... Jobs >
    Job& succeed(Job first, Jobs... rest)
    {
        succeed(first);
        if constexpr (sizeof...(rest) > 0) succeed(rest...);
        return *this;
    }

    /** Variadic: all listed jobs run after this job completes. */
    template< typename... Jobs >
    Job& precede(Job first, Jobs... rest)
    {
        precede(first);
        if constexpr (sizeof...(rest) > 0) precede(rest...);
        return *this;
    }

    /** @return true if this handle refers to a valid job node. */
    [[nodiscard]] constexpr bool valid() const noexcept { return idx_ != cInvalid; }

    /** @return true if this handle refers to a valid job node. */
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    /** @return true if both handles refer to the same job of the same Pipeline. */
    [[nodiscard]] constexpr bool operator==(Job other) const noexcept
    {
        return idx_ == other.idx_ && anchor_ == other.anchor_;
    }

    /**
     * @return This job's identifier within its Pipeline: the value observers
     *         receive and Pipeline::name(JobId) / successors(JobId) accept.
     * @note Meaningless for an invalid handle; check valid() first.
     */
    [[nodiscard]] constexpr JobId id() const noexcept { return idx_; }

    /**
     * @return The Pipeline this job belongs to, or nullptr for a
     *         default-constructed handle. Follows the Pipeline if it is moved.
     */
    [[nodiscard]] constexpr Pipeline* pipeline() const noexcept
    {
        return anchor_ ? anchor_->owner : nullptr;
    }

    /** Declare that this job runs AFTER every job in @p group. */
    Job& succeed(JobGroup const& group);

    /** Declare that every job in @p group runs AFTER this job. */
    Job& precede(JobGroup const& group);

private:
    friend class Pipeline;

    static constexpr uint32_t cInvalid = UINT32_MAX; ///< Sentinel for invalid handle.

    uint32_t                idx_{cInvalid};   ///< Index into Pipeline::Impl::nodes_.
    detail::PipelineAnchor* anchor_{nullptr}; ///< non-owning; the owning Pipeline's state

    constexpr explicit Job(uint32_t idx, detail::PipelineAnchor* anchor) noexcept
        : idx_{idx}, anchor_{anchor} {}
};

// ── Job status ────────────────────────────────────────────────────────────────

/** Runtime status of a single job node. */
enum class JobStatus : uint8_t
{
    kPending,   ///< Not yet started; waiting for dependencies.
    kReady,     ///< All dependencies met; queued for dispatch.
    kRunning,   ///< Currently executing.
    kDone,      ///< Completed successfully.
    kFailed,    ///< Completed with an error.
    kSkipped,   ///< Skipped because a required predecessor failed.
    kTimedOut,   ///< Exceeded the declared timeout.
    kCancelled,  ///< Cancelled externally via Job::cancel() or a stop token.
};

} // namespace sub0pipeline
