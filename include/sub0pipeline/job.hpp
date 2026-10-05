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

// ── Job handle ────────────────────────────────────────────────────────────────

/**
 * @brief Lightweight handle to a node in the Pipeline DAG.
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
     * @brief Set the maximum execution time for this job.
     *
     * If the job function does not return within `t`, the engine returns
     * `kTimeout` / `kTimedOut` and cascades skip to all successors.
     * The timed-out job remains owned until join_orphans(); functions must
     * also timeout at the syscall level (TCP connect, subprocess pipe).
     *
     * Default: `std::chrono::milliseconds::max()` (no timeout).
     */
    Job& timeout(std::chrono::milliseconds t) noexcept;

    /** Pin the job to a specific CPU core (-1 = any). */
    Job& core(int c) noexcept;

    /** Set the executor task stack size in bytes (default 8192). */
    Job& stack(uint32_t bytes) noexcept;

    /** Set the executor task priority 1–24 (default 5). */
    Job& priority(uint8_t p) noexcept;

    /** Ordinary failure does not block dependents; cancellation remains fatal. */
    Job& optional(bool opt = true) noexcept;

    /** Set status text shown while this job runs (for observer display). */
    Job& status(const char* text) noexcept;

    /**
     * @brief Request cancellation of this job.
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
     * @brief Declare that this job runs AFTER @p other completes.
     * @param other  The predecessor job.
     * @return *this for chaining.
     * @note May allocate (push_back on predecessor/successor vectors).
     */
    Job& succeed(Job other);

    /**
     * @brief Declare that @p other runs AFTER this job completes.
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

    /** @return true if both handles refer to the same job node. */
    [[nodiscard]] constexpr bool operator==(Job other) const noexcept { return idx_ == other.idx_; }

    /** @return pointer to the owning Pipeline (nullptr if default-constructed). */
    [[nodiscard]] constexpr Pipeline* pipeline() const noexcept { return pipeline_; }

    /** Declare that this job runs AFTER every job in @p group. */
    Job& succeed(JobGroup const& group);

    /** Declare that every job in @p group runs AFTER this job. */
    Job& precede(JobGroup const& group);

private:
    friend class Pipeline;

    static constexpr uint32_t cInvalid = UINT32_MAX; ///< Sentinel for invalid handle.

    uint32_t   idx_{cInvalid};     ///< Index into Pipeline::Impl::nodes_.
    Pipeline*  pipeline_{nullptr}; ///< Back-pointer to owning pipeline.

    constexpr explicit Job(uint32_t idx, Pipeline* p) noexcept
        : idx_{idx}, pipeline_{p} {}
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

/** Stable node index within one Pipeline. Graphs accept at most 65,536 jobs,
 * and one job at most 32,767 successors; exceeding either is a hard error.
 * Append-only node indices remain valid until their owning Pipeline is destroyed.
 */
using JobId = uint32_t;

} // namespace sub0pipeline
