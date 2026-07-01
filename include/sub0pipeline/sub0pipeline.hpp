// include/sub0pipeline/sub0pipeline.hpp
//
// Sub0Pipeline — Lightweight C++23 DAG job scheduler.
//
// Declare jobs and their dependencies as a directed acyclic graph, then
// execute them in parallel via a platform-injectable executor. Independent
// jobs run concurrently; dependent jobs wait for their predecessors.
//
// Design principles:
//   - Graph-as-value: the DAG is a first-class inspectable object
//   - Builder pattern: fluent .precede()/.name() chaining on Job handles
//   - Observer hooks: pluggable onStart/onFinish for profiling and progress
//   - Platform-injectable executor: pluggable backends (threaded, sequential, RTOS)
//   - Zero-overhead when jobs are constexpr-declared
//
// Usage:
//   sub0pipeline::Pipeline pipe;
//   auto a = pipe.emplace([] { return init_a(); }).name("A");
//   auto b = pipe.emplace([] { return init_b(); }).name("B").timeout(8s);
//   auto c = pipe.emplace([] { return init_c(); }).name("C").timeout(10s);
//   auto d = pipe.emplace([] { return start_d(); }).name("D");
//   d.succeed(b, c);   // D depends on both B and C
//   // B and C have no mutual dependency — run in parallel
//   pipe.run(executor, &observer);
//
#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

// ── Compile-time error-handling policy ───────────────────────────────────────
// Define SUB0PIPELINE_EXCEPTIONS=1 (default on most desktop platforms) to throw
// std::runtime_error on hard errors (e.g. pool address overflow).
// Define SUB0PIPELINE_EXCEPTIONS=0 to call std::terminate instead (RTOS, bare-metal,
// or builds with -fno-exceptions).
//
// CMake: option(SUB0PIPELINE_EXCEPTIONS "Throw std::runtime_error on hard errors" ON)
//        then: target_compile_definitions(Sub0Pipeline PUBLIC SUB0PIPELINE_EXCEPTIONS=$<BOOL:${SUB0PIPELINE_EXCEPTIONS}>)
#if !defined(SUB0PIPELINE_EXCEPTIONS)
#  if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#    define SUB0PIPELINE_EXCEPTIONS 1
#  else
#    define SUB0PIPELINE_EXCEPTIONS 0
#  endif
#endif

#if SUB0PIPELINE_EXCEPTIONS
#  define SUB0PIPELINE_THROW(msg) throw ::std::runtime_error(msg)
#else
#  define SUB0PIPELINE_THROW(msg) ::std::terminate()
#endif

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
    kNotArmed,          ///< trigger() called before arm() -- no executor stored.
    kNotOnDemand,       ///< trigger() called on a job not registered via add_on_demand().
    kCancelled,         ///< Job was cancelled externally via Job::cancel() or a stop token.
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

    /** Set a human-readable name (used in tracing and observer callbacks). */
    Job& name(std::string_view n);

    /**
     * @brief Set the maximum execution time for this job.
     *
     * If the job function does not return within `t`, the engine returns
     * `kTimeout` / `kTimedOut` and cascades skip to all successors.
     * The job thread is detached -- UDAW job functions are expected to
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

    /** Mark as optional: failure does not block or skip dependents. */
    Job& optional(bool opt = true) noexcept;

    /** Set status text shown while this job runs (for observer display). */
    Job& status(const char* text) noexcept;

    /**
     * @brief Request cancellation of this job.
     *
     * Thread-safe. Fires the job's `std::stop_source`, setting its
     * `stop_token` to stopped. Cancellable job functions (those taking
     * `std::stop_token`) check `stop_requested()` and return `kCancelled`.
     * Non-cancellable functions ignore the signal; timeout enforcement
     * remains active.
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
    kTimedOut,   ///< Exceeded the declared timeout.
    kCancelled,  ///< Cancelled externally via Job::cancel() or a stop token.
};

// ── Executor interface ────────────────────────────────────────────────────────

/**
 * @brief Platform-injectable execution backend.
 *
 * Provides an abstraction layer so the same Pipeline DAG engine runs on
 * any platform: threaded, sequential/inline, or RTOS-based.
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

private:
    IExecutor&                parent_;
    std::atomic<uint32_t>     localInFlight_{0U};
    std::mutex                mtx_;
    std::condition_variable   cv_;
};

// ── Observer interface ────────────────────────────────────────────────────────

/**
 * @brief Pluggable observer for profiling and progress tracking.
 *
 * Attach via Pipeline::run() to receive callbacks as jobs start and finish.
 */
class IObserver
{
public:
    virtual ~IObserver() = default;

    /** Called just before a job starts executing (hot path -- keep implementations fast). */
    virtual void onStart(std::string_view jobName) = 0;

    /**
     * @brief Called when a job completes (any terminal status).
     * @param jobName   The job's name.
     * @param status    Final status (kDone, kFailed, kSkipped, kTimedOut).
     * @param progress  Fraction of total jobs completed (0.0–1.0).
     */
    virtual void onFinish(std::string_view jobName, JobStatus status, float progress) = 0;

    /**
     * @brief Called when a non-optional job fails -- default no-op.
     *
     * Separate from onFinish so callers only opt into failure detail when they
     * need it.  Zero cost when the default no-op is not overridden; the
     * observer vtable dispatch itself is already gated by `if (observer)`.
     *
     * @param jobName  Name of the failed job.
     * @param error    The PipelineError code.
     * @param message  Diagnostic string set by the job via
     *                 Pipeline::set_current_job_error() -- empty if the job
     *                 did not provide context.  Only allocated on the failure
     *                 branch; the success hot-path never touches this string.
     */
    virtual void onFailure([[maybe_unused]] std::string_view jobName,
                           [[maybe_unused]] PipelineError    error,
                           [[maybe_unused]] std::string_view message) noexcept {}

    /** Called when a dependency edge is traversed (for Gantt chart arrows). */
    virtual void onDependency([[maybe_unused]] std::string_view from,
                              [[maybe_unused]] std::string_view to) {}
};

// ── Tick job ──────────────────────────────────────────────────────────────────

/** Recurring task registered for the tick event loop. */
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
     * @brief Add a cancellable job whose function receives a `std::stop_token`.
     *
     * The stop token is signalled when:
     *   - The job's `.timeout()` expires (cooperative signal before hard cutoff).
     *   - `Job::cancel()` is called from any thread.
     *
     * The function should poll `token.stop_requested()` at checkpoints and
     * return `std::unexpected(PipelineError::kCancelled)` when it fires:
     * @code
     *   pipe.emplace([](std::stop_token st) -> std::expected<void, PipelineError> {
     *       while (!st.stop_requested()) {
     *           if (!fetch_chunk()) break;
     *       }
     *       if (st.stop_requested())
     *           return std::unexpected(PipelineError::kCancelled);
     *       return {};
     *   }).name("fetch").timeout(5s);
     * @endcode
     */
    [[nodiscard]] Job emplace(
        std::function<std::expected<void, PipelineError>(std::stop_token)> fn);

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
     * Re-runnable: calling run() again re-executes the entire DAG using an
     * epoch-based reset — no separate reset() call is needed. Each run
     * increments an internal epoch; nodes lazily reset their state when
     * first touched, avoiding an O(N) bulk reset pass.
     *
     * @param executor  Execution backend (threaded, sequential, custom, …).
     * @param observer  Optional observer for progress and tracing.
     * @return          std::expected<void, PipelineError> — empty on success,
     *                  or the first fatal error encountered.
     */
    [[nodiscard]] auto run(IExecutor& executor, IObserver* observer = nullptr)
        -> std::expected<void, PipelineError>;

    /**
     * @brief Run the pipeline synchronously on the calling thread (no executor required).
     *
     * Convenience overload that creates an inline sequential executor internally.
     * Jobs execute in dependency order on the calling thread with no parallelism.
     * Useful for request-scoped pipelines, tests, and embedded contexts where
     * creating an executor explicitly would be boilerplate.
     *
     * @code
     *   Pipeline pipe;
     *   pipe >> "parse"_job(parse) >> "validate"_job(validate) >> "commit"_job(commit);
     *   auto result = pipe.run_inline();   // no executor needed
     * @endcode
     */
    [[nodiscard]] auto run_inline(IObserver* observer = nullptr)
        -> std::expected<void, PipelineError>;

    /** @return Current status of a job (kPending before run()). */
    [[nodiscard]] auto status(Job j) const noexcept -> JobStatus;

    /** @return Human-readable name of a job. */
    [[nodiscard]] auto name(Job j) const noexcept -> std::string_view;

    /**
     * @brief Name of the first non-optional job that failed in the most recent run().
     *
     * Empty string if the last run() succeeded or has not been called yet.
     * Useful for error reporting without requiring an IObserver:
     * @code
     *   auto r = pipe.run(exec);
     *   if (!r) fmt::print("Failed job: {}\n", pipe.first_failure_name());
     * @endcode
     */
    [[nodiscard]] std::string_view first_failure_name() const noexcept;

    /**
     * @brief Set a diagnostic message for the job currently executing on this thread.
     *
     * Call on the failure branch before returning `std::unexpected(...)`.  The
     * message is consumed by `dispatchJob` and forwarded to `IObserver::onFailure`.
     * Zero cost on the success path -- the thread-local string is never read when
     * the job succeeds.
     *
     * @code
     *   auto fn = [&]() -> std::expected<void, PipelineError> {
     *       if (!connect()) {
     *           Pipeline::set_current_job_error("TCP connect timed out after 30s");
     *           return std::unexpected(PipelineError::kJobFailed);
     *       }
     *       return {};
     *   };
     * @endcode
     */
    static void set_current_job_error(std::string_view msg) noexcept;

    /**
     * @brief Lightweight status snapshot of all jobs (for status bars and UI).
     *
     * Each `JobSnapshot` is a {name, status} pair read via relaxed atomic load --
     * no locks, no synchronisation barrier.  Safe to call from any thread at any
     * time; results are a consistent point-in-time view of the per-job atomics.
     * Allocates one `std::vector` per call; poll at the display frame rate (not
     * tighter than 16 ms) to avoid unnecessary pressure.
     *
     * For single-string "current job" display prefer `IObserver::onStart` feeding
     * an atomic pointer -- zero allocation, zero polling.
     */
    struct JobSnapshot {
        std::string_view name;    ///< Stable pointer into the pipeline's node; valid until pipeline is destroyed.
        JobStatus        status;  ///< Relaxed atomic load of the job's current status.
    };
    [[nodiscard]] std::vector<JobSnapshot> snapshot() const noexcept;

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

    // ── Tick loop ─────────────────────────────────────────────────────────

    /** Register a recurring job for the tick event loop. */
    void add_tick(TickJob tick);

    /**
     * @brief Enter the main event loop — runs tick jobs at their intervals.
     *
     * Does not return. Call after run() completes to begin steady-state
     * operation. Yields between iterations using a platform-appropriate
     * sleep (1 ms).
     */
    [[noreturn]] void run_loop();

    /**
     * @brief Re-run the pipeline repeatedly until the stop token is signalled.
     *
     * Each iteration calls run(executor) and discards the result. Useful for
     * perpetual update loops (game frame loop, streaming processor, background
     * worker) where the same DAG structure executes at continuous intervals.
     *
     * The caller controls pacing -- insert a sleep or frame-sync between
     * iterations in the job functions or by wrapping this call:
     * @code
     *   std::jthread worker([&](std::stop_token st) {
     *       pipe.run_until(exec, st, observer,
     *           [&](PipelineError e) { reconnect(); });
     *   });
     * @endcode
     *
     * @param observer  Optional observer forwarded to each run() call.
     * @param onError   Optional callback invoked when run() returns a fatal
     *                  error. The callback may call stop.request_stop() on the
     *                  outer jthread to abort the loop on unrecoverable errors.
     */
    void run_until(IExecutor& executor, std::stop_token stop,
                   IObserver* observer = nullptr,
                   std::function<void(PipelineError)> onError = nullptr);

    // ── On-demand jobs ────────────────────────────────────────────────────

    /**
     * @brief Arm the pipeline with an executor for on-demand dispatch.
     *
     * Must be called before trigger(). The executor and observer are stored
     * by pointer; the caller must keep them alive for the lifetime of any
     * trigger() calls. Does not transfer ownership.
     *
     * Typical usage:
     * @code
     *   auto exec = makeDesktopExecutor();
     *   pipeline.arm(*exec);
     *   // ... later from any thread:
     *   pipeline.trigger(job);
     * @endcode
     */
    void arm(IExecutor& executor, IObserver* observer = nullptr) noexcept;

    /**
     * @brief Register a job that is excluded from normal run() execution
     *        and dispatched only when trigger() is called.
     *
     * On-demand jobs are not included in the root set for run() -- they do
     * not execute during the normal DAG execution phase. Call arm() with an
     * executor before calling trigger().
     */
    [[nodiscard]] Job add_on_demand(std::function<std::expected<void, PipelineError>()> fn);

    /**
     * @brief Register a cancellable on-demand job (receives `std::stop_token`).
     *
     * Equivalent to `add_on_demand()` but the function is called with the job's
     * stop token, enabling cooperative cancellation via `Job::cancel()` or
     * `.timeout()`. Sets `kFlagCancellable` so the watchdog path is used rather
     * than the hard packaged_task cutoff.
     *
     * Primary use case: UDAW FetchQueue drainer that must exit cleanly when
     * a client session disconnects.
     */
    [[nodiscard]] Job add_on_demand(
        std::function<std::expected<void, PipelineError>(std::stop_token)> fn);

    /**
     * @brief Dispatch an on-demand job via the armed executor.
     *
     * Safe to call from any thread. Requires arm() to have been called first.
     * Returns an error if the pipeline has not been armed or the job is not
     * an on-demand job.
     *
     * The job executes asynchronously; completion is reported via the observer
     * passed to arm() (if any).
     */
    [[nodiscard]] std::expected<void, PipelineError> trigger(Job j);

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

// ── Executor factory functions ────────────────────────────────────────────────

/// Returns a `DesktopExecutor` (one `std::thread` per job, no priority ordering).
/// Defined in `platform/desktop/`.
std::unique_ptr<IExecutor> makeDesktopExecutor();

/// Returns a `SequentialExecutor` (inline, no threads, deterministic).
/// Defined in `platform/headless/`.
std::unique_ptr<IExecutor> makeSequentialExecutor();

/**
 * @brief Returns a `PriorityExecutor` -- bounded thread pool that dispatches
 *        higher-priority jobs first.
 *
 * Jobs with a larger `.priority()` value are started before lower-priority
 * jobs that are still queued. Running jobs are not preempted.
 *
 * Typical UDAW usage: blocking fetches (.priority(10)) preempt prefetch
 * hints (.priority(5)) when the thread pool is under load.
 *
 * @param threadCount  Worker thread count. 0 = `hardware_concurrency()`.
 *
 * Defined in `platform/priority/`.
 */
std::unique_ptr<IExecutor> makePriorityExecutor(unsigned int threadCount = 0);

} // namespace sub0pipeline
