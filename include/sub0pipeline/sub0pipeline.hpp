// include/sub0pipeline/sub0pipeline.hpp
//
// Sub0Pipeline — Zero-overhead DAG job scheduler for boot sequencing and
// application lifecycle management. Runs on FreeRTOS/C++23/ESP32-P4 and
// desktop platforms via a platform-injectable executor.
//
// Inspired by Taskflow's DAG model, extracted from NestNinja (ADR-027) as
// a standalone Sub0-family library.
//
// Design principles:
//   - Graph-as-value: the DAG is a first-class inspectable object
//   - Builder pattern: fluent .precede()/.name() chaining on Job handles
//   - Observer hooks: pluggable on_start/on_finish (profiling, boot screen)
//   - Platform-injectable executor: FreeRTOS tasks / std::thread / sequential
//   - Zero-overhead when jobs are constexpr-declared
//
// Usage:
//   sub0pipeline::Pipeline boot;
//   auto nvs     = boot.emplace([] { return nvs_init(); }).name("nvs");
//   auto display = boot.emplace([] { return display_init(); }).name("display").timeout(8s);
//   auto network = boot.emplace([] { return network_init(); }).name("network").timeout(10s);
//   auto storage = boot.emplace([] { return storage_init(); }).name("storage");
//   storage.succeed(nvs);   // storage depends on nvs
//   // display and network have no dependency — run in parallel
//   boot.run(executor, &observer);
//
#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sub0pipeline {

// ── Error types ──────────────────────────────────────────────────────────────

/** Error codes returned by Pipeline operations. */
enum class PipelineError : uint8_t
{
    kTimeout,           ///< Job exceeded its declared timeout.
    kJobFailed,         ///< Job function returned an unexpected error.
    kDependencyFailed,  ///< A required predecessor job failed.
    kCyclicDependency,  ///< The DAG contains a cycle.
    kDuplicateJob,      ///< Job was added more than once.
    kUnknownJob,        ///< Operation on an invalid Job handle.
};

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

    /** Set a human-readable name (used in tracing and boot screen). */
    Job& name(std::string_view n) noexcept;

    /** Set maximum execution time before the job is considered timed out. */
    Job& timeout(std::chrono::milliseconds t) noexcept;

    /** Pin the job to a specific CPU core (-1 = any). */
    Job& core(int c) noexcept;

    /** Set the FreeRTOS task stack size in bytes (default 8192). */
    Job& stack(uint32_t bytes) noexcept;

    /** Set the FreeRTOS task priority 1–24 (default 5). */
    Job& priority(uint8_t p) noexcept;

    /** Mark as optional: failure does not block or skip dependents. */
    Job& optional(bool opt = true) noexcept;

    /** Set boot-screen status text shown while this job runs. */
    Job& status(const char* text) noexcept;

    /**
     * @brief Declare that this job runs AFTER @p other completes.
     * @param other  The predecessor job.
     * @return *this for chaining.
     */
    Job& succeed(Job other) noexcept;

    /**
     * @brief Declare that @p other runs AFTER this job completes.
     * @param other  The successor job.
     * @return *this for chaining.
     */
    Job& precede(Job other) noexcept;

    /** Variadic: this job runs after all listed jobs complete. */
    template< typename... Jobs >
    Job& succeed(Job first, Jobs... rest) noexcept
    {
        succeed(first);
        if constexpr (sizeof...(rest) > 0) succeed(rest...);
        return *this;
    }

    /** Variadic: all listed jobs run after this job completes. */
    template< typename... Jobs >
    Job& precede(Job first, Jobs... rest) noexcept
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

private:
    friend class Pipeline;

    static constexpr uint32_t cInvalid = UINT32_MAX; ///< Sentinel for invalid handle.

    uint32_t   idx_{cInvalid};    ///< Index into Pipeline::Impl::nodes_.
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
    kTimedOut,  ///< Exceeded the declared timeout.
};

// ── Executor interface ────────────────────────────────────────────────────────

/**
 * @brief Platform-injectable execution backend.
 *
 * Provides an abstraction layer so the same Pipeline DAG engine runs on
 * FreeRTOS (ESP32-P4), std::thread (desktop), or inline (headless/tests).
 *
 * Contract:
 *   - dispatch() MUST eventually call on_complete() from any context.
 *   - wait_all() MUST NOT return until all dispatched callbacks have fired.
 */
class IExecutor
{
public:
    virtual ~IExecutor() = default;

    /**
     * @brief Dispatch a job for asynchronous execution.
     * @param name         Human-readable label (for logging).
     * @param fn           The job function to execute.
     * @param on_complete  Callback fired when fn returns (required).
     * @param core_affinity CPU core hint (-1 = any).
     * @param priority     Scheduling priority (1–24).
     * @param stack_bytes  Stack allocation for embedded targets.
     */
    virtual void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         on_complete,
        int                           core_affinity,
        uint8_t                       priority,
        uint32_t                      stack_bytes = 4096U) = 0;

    /** Block until all previously dispatched jobs have called on_complete(). */
    virtual void wait_all() = 0;

    /** @return Number of parallel execution slots (cores / thread pool size). */
    [[nodiscard]] virtual int concurrency() const noexcept = 0;
};

// ── Observer interface ────────────────────────────────────────────────────────

/**
 * @brief Pluggable observer for profiling, progress tracking, and boot screen.
 *
 * Inspired by Taskflow's tf::ObserverInterface. Attach via Pipeline::run().
 */
class IObserver
{
public:
    virtual ~IObserver() = default;

    /** Called just before a job starts executing. */
    virtual void on_start(std::string_view job_name) = 0;

    /**
     * @brief Called when a job completes (success or failure).
     * @param job_name  The job's name.
     * @param status    Final status (kDone, kFailed, kSkipped, kTimedOut).
     * @param progress  Fraction of total jobs completed (0.0–1.0).
     */
    virtual void on_finish(std::string_view job_name, JobStatus status, float progress) = 0;

    /** Called when a dependency edge is traversed (for Gantt chart arrows). */
    virtual void on_dependency([[maybe_unused]] std::string_view from,
                               [[maybe_unused]] std::string_view to) {}
};

// ── Tick job ──────────────────────────────────────────────────────────────────

/** Recurring task registered for the post-boot event loop. */
struct TickJob
{
    std::string_view          name;      ///< Human-readable label.
    std::chrono::milliseconds interval;  ///< Minimum period (0 = every iteration).
    std::function<void()>     fn;        ///< Function to call each interval.
};

// ── Pipeline ──────────────────────────────────────────────────────────────────

/**
 * @brief DAG-based job scheduler.
 *
 * Owns all job nodes and their dependency edges. Jobs are emplaced during a
 * build phase, then executed in dependency order via run(). Independent jobs
 * are dispatched in parallel by the injected IExecutor.
 *
 * Thread safety: the build phase (emplace/succeed/precede) is single-threaded.
 * After run() completes, status() and name() are safe to call from any thread.
 */
class Pipeline
{
public:
    Pipeline();
    ~Pipeline();

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) noexcept        = default;
    Pipeline& operator=(Pipeline&&)      = default;

    // ── DAG construction ─────────────────────────────────────────────────

    /**
     * @brief Add a job that returns std::expected<void, PipelineError>.
     * @param fn  The job function.
     * @return    A Job handle for setting name, timeouts, and dependencies.
     */
    Job emplace(std::function<std::expected<void, PipelineError>()> fn);

    /**
     * @brief Add a void-returning job (always succeeds).
     * @param fn  The job function.
     * @return    A Job handle for setting name, timeouts, and dependencies.
     */
    Job emplace_void(std::function<void()> fn);

    /**
     * @brief Convenience overload: add a void-returning lambda.
     *
     * The concept constraint prevents this from matching expected-returning
     * callables — those are routed to the std::function overload above.
     */
    template< typename F >
        requires std::invocable<F> && std::same_as<std::invoke_result_t<F>, void>
    Job emplace(F&& f)
    {
        return emplace_void(std::forward<F>(f));
    }

    /** @return Total number of jobs currently in the DAG. */
    [[nodiscard]] std::size_t size() const noexcept;

    // ── Execution ────────────────────────────────────────────────────────

    /**
     * @brief Execute all jobs in dependency order, parallelising independent jobs.
     *
     * Validates the DAG, seeds root jobs, then dispatches successors as their
     * predecessors complete. Blocks until all jobs finish or a required job fails.
     *
     * @param executor  Platform executor (desktop, FreeRTOS, sequential, …).
     * @param observer  Optional observer for progress and tracing.
     * @return          std::expected<void, PipelineError> — empty on success,
     *                  or the first fatal error encountered.
     */
    [[nodiscard]] auto run(IExecutor& executor, IObserver* observer = nullptr)
        -> std::expected<void, PipelineError>;

    /** @return Current status of a job (kPending before run()). */
    [[nodiscard]] auto status(Job j) const noexcept -> JobStatus;

    /** @return Human-readable name of a job. */
    [[nodiscard]] auto name(Job j) const noexcept -> std::string_view;

    // ── Validation ───────────────────────────────────────────────────────

    /**
     * @brief Validate the DAG before execution.
     *
     * Uses Kahn's algorithm to detect cycles. Called automatically by run(),
     * but can be called explicitly during the build phase.
     *
     * @return std::expected<void, PipelineError::kCyclicDependency> on cycle.
     */
    [[nodiscard]] auto validate() const -> std::expected<void, PipelineError>;

    // ── Post-boot tick loop ───────────────────────────────────────────────

    /** Register a recurring job for the post-boot event loop. */
    void add_tick(TickJob tick);

    /**
     * @brief Enter the main event loop — runs tick jobs at their intervals.
     *
     * Does not return. Call after run() completes to begin steady-state
     * operation. On FreeRTOS, yields via vTaskDelay(1); on desktop, via
     * std::this_thread::sleep_for(1ms).
     */
    [[noreturn]] void run_loop();

    // ── On-demand jobs ────────────────────────────────────────────────────

    /** Register a job that can be triggered by an event post-boot. */
    Job add_on_demand(std::function<std::expected<void, PipelineError>()> fn);

    /** Trigger an on-demand job. Safe to call from any task or ISR. */
    void trigger(Job j);

    // ── Diagnostics ───────────────────────────────────────────────────────

    /** Emit DAG structure as trace events (Perfetto Gantt chart). */
    void dump_trace() const;

    /** Print the DAG as a human-readable dependency list to stdout. */
    void dump_text() const;

private:
    struct Node;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class Job;

    Node&       node(uint32_t idx);
    const Node& node(uint32_t idx) const;
};

} // namespace sub0pipeline
