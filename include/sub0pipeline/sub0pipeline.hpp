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
//   - Observer hooks: pluggable onStart/onFinish (profiling, boot screen)
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
//   // display and network have no mutual dependency — run in parallel
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
#include <tuple>
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

// Forward declarations for cross-references.
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

    /** Set a human-readable name (used in tracing and boot screen). */
    Job& name(std::string_view n);

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
    Job& succeed(class JobGroup const& group);

    /** Declare that every job in @p group runs AFTER this job. */
    Job& precede(class JobGroup const& group);

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
 *   - dispatch() MUST increment its in-flight counter before returning.
 *   - dispatch() MUST eventually call onComplete() from the dispatched context.
 *   - wait_all() MUST NOT return until all dispatched onComplete() calls have fired.
 */
class IExecutor
{
public:
    virtual ~IExecutor() = default;

    /**
     * @brief Dispatch a job for asynchronous execution.
     * @param name         Human-readable label (for logging).
     * @param fn           The job function to execute.
     * @param onComplete   Callback fired when fn returns (required by contract).
     * @param coreAffinity CPU core hint (-1 = any).
     * @param priority     Scheduling priority (1–24).
     * @param stackBytes   Stack allocation for embedded targets.
     */
    virtual void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           coreAffinity,
        uint8_t                       priority,
        uint32_t                      stackBytes = 4096U) = 0;

    /** Block until all previously dispatched jobs have called onComplete(). */
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
    virtual void onStart(std::string_view jobName) = 0;

    /**
     * @brief Called when a job completes (any terminal status).
     * @param jobName   The job's name.
     * @param status    Final status (kDone, kFailed, kSkipped, kTimedOut).
     * @param progress  Fraction of total jobs completed (0.0–1.0).
     */
    virtual void onFinish(std::string_view jobName, JobStatus status, float progress) = 0;

    /** Called when a dependency edge is traversed (for Gantt chart arrows). */
    virtual void onDependency([[maybe_unused]] std::string_view from,
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
 *
 * Post-move state: after a move, the Pipeline is empty but valid; calling
 * emplace() on a moved-from Pipeline recreates the internal state.
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
     * @note      The returned handle should not be discarded if dependencies
     *            or metadata need to be set.
     */
    [[nodiscard]] Job emplace(std::function<std::expected<void, PipelineError>()> fn);

    /**
     * @brief Add a void-returning job (always succeeds).
     * @param fn  The job function.
     * @return    A Job handle for setting name, timeouts, and dependencies.
     */
    [[nodiscard]] Job emplace_void(std::function<void()> fn);

    /**
     * @brief Convenience overload: add a void-returning lambda.
     *
     * The concept constraint prevents this from matching expected-returning
     * callables — those are routed to the std::function overload above.
     */
    template< typename F >
        requires std::invocable<F> && std::same_as<std::invoke_result_t<F>, void>
    [[nodiscard]] Job emplace(F&& f)
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
     * @return std::unexpected(PipelineError::kCyclicDependency) on cycle.
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

    /**
     * @brief Register a job that can be triggered by an event post-boot.
     * @note trigger() is currently a stub; the returned Job is not yet
     *       connected to the event-dispatch mechanism.
     */
    [[nodiscard]] Job add_on_demand(std::function<std::expected<void, PipelineError>()> fn);

    /**
     * @brief Trigger an on-demand job. Safe to call from any task or ISR.
     * @note Currently a stub — see PLATFORM_ROADMAP.md.
     */
    void trigger(Job j);

    // ── Generic emplace (concept-based extension point) ─────────────────

    /**
     * @brief Emplace a job described by a spec object with a .build() method.
     *
     * Accepts any type satisfying: `spec.build(Pipeline&) -> Job`.
     * This is the extension point used by the DSL's JobSpec type.
     */
    template<typename Spec>
        requires requires(Spec& s, Pipeline& p) { { s.build(p) } -> std::same_as<Job>; }
    [[nodiscard]] Job emplace(Spec&& spec)
    {
        return std::forward<Spec>(spec).build(*this);
    }

    /**
     * @brief Multi-emplace returning a tuple for structured bindings.
     * @example auto [a, b, c] = pipe.emplace(specA, specB, specC);
     */
    template<typename... Specs>
        requires (sizeof...(Specs) > 1)
              && (requires(Specs& s, Pipeline& p) { { s.build(p) } -> std::same_as<Job>; } && ...)
    [[nodiscard]] auto emplace(Specs&&... specs)
    {
        return std::tuple{std::forward<Specs>(specs).build(*this)...};
    }

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

// ── JobGroup ────────────────────────────────────────────────────────────────

/**
 * @brief A named group of parallel Job handles.
 *
 * Provides .succeed() and .precede() that delegate to every member,
 * allowing a group to be wired as a single unit in dependency expressions.
 * Created via parallel() or DSL operator+.
 */
class JobGroup
{
public:
    JobGroup() = default;

    /** Construct from two jobs. */
    explicit JobGroup(Job first, Job second)
        : jobs_{first, second} {}

    /** Add a job to the group. Returns *this for chaining. */
    JobGroup& add(Job j) { jobs_.push_back(j); return *this; }

    /** Every job in this group runs AFTER @p other. */
    JobGroup& succeed(Job other);

    /** Every job in this group runs AFTER every job in @p other. */
    JobGroup& succeed(JobGroup const& other);

    /** Variadic: every job in this group runs AFTER all listed jobs. */
    template<typename... Jobs>
    JobGroup& succeed(Job first, Jobs... rest)
    {
        succeed(first);
        if constexpr (sizeof...(rest) > 0) succeed(rest...);
        return *this;
    }

    /** Every job in @p other runs AFTER every job in this group. */
    JobGroup& precede(Job other);

    /** Every job in @p other group runs AFTER every job in this group. */
    JobGroup& precede(JobGroup const& other);

    /** Variadic: all listed jobs run AFTER every job in this group. */
    template<typename... Jobs>
    JobGroup& precede(Job first, Jobs... rest)
    {
        precede(first);
        if constexpr (sizeof...(rest) > 0) precede(rest...);
        return *this;
    }

    /** Read-only view of member jobs. */
    [[nodiscard]] const std::vector<Job>& jobs() const noexcept { return jobs_; }

private:
    std::vector<Job> jobs_;
};

/**
 * @brief Create a group of parallel jobs.
 * @example auto io = parallel(display, network, audio);
 */
template<typename... Jobs_t>
    requires (std::same_as<std::remove_cvref_t<Jobs_t>, Job> && ...)
[[nodiscard]] JobGroup parallel(Jobs_t... jobs)
{
    JobGroup g;
    (g.add(jobs), ...);
    return g;
}

} // namespace sub0pipeline
