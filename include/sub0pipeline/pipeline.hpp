// include/sub0pipeline/pipeline.hpp
//
// Pipeline — the DAG-based job scheduler: graph construction, execution,
// validation, tick loop and on-demand jobs.
#pragma once

#include <sub0pipeline/dependency_range.hpp>
#include <sub0pipeline/error.hpp>
#include <sub0pipeline/executor/executor.hpp>
#include <sub0pipeline/job.hpp>
#include <sub0pipeline/observer.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iosfwd>
#include <memory>
#include <stop_token>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace sub0pipeline {

class IDeadlineService;

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
 * emplace() on a moved-from Pipeline recreates the internal state. Job handles
 * stay valid across a move and then refer to the destination. Move only while
 * idle: not during a run, a trigger or while orphaned work remains.
 */
class Pipeline
{
public:
    Pipeline();
    ~Pipeline();

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    // Defined out of line: moving needs the complete implementation type.
    Pipeline(Pipeline&&) noexcept;
    Pipeline& operator=(Pipeline&&);

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
     *   - The job's `.timeout()` expires (cooperative stop request; the body must return).
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
     * @brief Add a void-returning callable (always succeeds).
     *
     * The callable is stored directly, without an intermediate std::function,
     * so a small one costs no extra allocation and one indirect call per run.
     */
    template< typename F >
        requires std::invocable<F> && std::same_as<std::invoke_result_t<F>, void>
    [[nodiscard]] Job emplace(F&& f)
    {
        return emplacePlain(
            [fn = std::forward<F>(f)](std::stop_token) mutable
                -> std::expected<void, PipelineError> { fn(); return {}; });
    }

    /**
     * @brief Add a callable returning std::expected<void, PipelineError>.
     *
     * Stored directly, as above. A std::function object passed as such still
     * selects the std::function overload.
     */
    template< typename F >
        requires std::invocable<F>
              && std::same_as<std::invoke_result_t<F>, std::expected<void, PipelineError>>
    [[nodiscard]] Job emplace(F&& f)
    {
        return emplacePlain(
            [fn = std::forward<F>(f)](std::stop_token) mutable { return fn(); });
    }

    /**
     * @brief Reserve storage for at least @p jobCount jobs.
     *
     * Optional. Avoids moving existing jobs as the graph grows; it does not
     * reserve the callables, names or wide successor lists that jobs own.
     * Build phase only, like emplace().
     */
    void reserve(std::size_t jobCount);

    /** @return Total number of jobs currently in the DAG. */
    [[nodiscard]] std::size_t size() const noexcept;

    /** Borrow an optional deadline service. Set only while idle, with no
     * pending callbacks or orphans. nullptr restores native timeout helpers.
     * Cooperative timed bodies use no helper thread with an injected service;
     * plain timed bodies still need a native worker to enforce a cutoff.
     */
    void set_deadline_service(IDeadlineService* service);

    // ── Execution ────────────────────────────────────────────────────────

    /**
     * @brief Execute all jobs in dependency order, parallelising independent jobs.
     *
     * Validates the DAG, seeds root jobs, then dispatches successors as their
     * predecessors complete. Blocks until all jobs finish or a required job fails.
     *
     * Re-runnable: calling run() again re-executes the entire DAG using an
     * fresh cancellation/dependency state before dispatch. No separate reset()
     * call is needed. Validation is cached until the topology changes.
     *
     * @param executor  Execution backend (threaded, sequential, custom, …).
     * @param observer  Optional observer for progress and tracing.
     * @return          std::expected<void, PipelineError> — empty on success,
     *                  or the first fatal error encountered.
     */
    [[nodiscard]] auto run(IExecutor& executor, IObserver* observer = nullptr)
        -> std::expected<void, PipelineError>;

    /**
     * Execute with external cancellation. Pending jobs check cancellation before
     * invoking their body, including functions without a stop-token parameter.
     * Running cooperative jobs receive the request through their per-job token.
     * A request racing with body entry may be observed by the body instead.
     * Cancellation is fatal even for optional jobs; successors are skipped.
     * A successful durable commit is not rolled back when its ACK is suppressed.
     * Consumers must make retries idempotent using durable operation identifiers.
     *
     * run() waits for executor callbacks, but timed-out non-cooperative jobs may
     * remain: call join_orphans() before releasing borrowed state. Subsequent
     * runs join previous orphans before resetting state; concurrent runs return
     * kBusy. The executor, observer, Pipeline and borrowed state must remain
     * alive until run() and join_orphans() have completed. Owner teardown must
     * request stop and join its run thread before destroying those members.
     */
    [[nodiscard]] auto run(IExecutor& executor, std::stop_token external,
                           IObserver* observer = nullptr)
        -> std::expected<void, PipelineError>;

    /**
     * @brief Run the pipeline synchronously on the calling thread (no executor required).
     *
     * Convenience overload that creates an inline sequential executor internally.
     * Untimed jobs execute in dependency order on the calling thread. Timeout
     * enforcement may create helper threads; join_orphans() still applies.
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

    /** Execute with external cancellation using the inline executor.
     * Timed jobs may use helper threads; the run()/join_orphans() contract applies.
     */
    [[nodiscard]] auto run_inline(std::stop_token external, IObserver* observer = nullptr)
        -> std::expected<void, PipelineError>;

    /** Join timed-out non-cooperative jobs after run() or executor.wait_all().
     * May block indefinitely if a job never returns. Concurrent joiners are
     * serialized. Do not call from a job, or start new work during teardown.
     * Returns whether this call reaped any threads.
     */
    bool join_orphans();

    /** True while timed-out threads remain unreaped, including during a join.
     * Thread-safe query, not a substitute for joining or synchronizing producers.
     */
    [[nodiscard]] bool has_pending_orphans() const noexcept;

    /** @return Current status of a job (kPending before run()). */
    [[nodiscard]] auto status(Job j) const noexcept -> JobStatus;

    /** @return Human-readable name of a job. */
    [[nodiscard]] auto name(Job j) const noexcept -> std::string_view;
    /** Return a borrowed name for a stable node id, or an empty view if invalid.
     * Concurrent reads require a stable graph with no renaming or mutation.
     */
    [[nodiscard]] auto name(JobId id) const noexcept -> std::string_view;
    /** Return a non-owning view of a node's successors, empty for invalid ids.
     * Concurrent reads require a stable graph; graph edits or destruction
     * invalidate the view and its iterators.
     */
    [[nodiscard]] auto successors(JobId id) const noexcept -> DependencyRange;

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
     * message is consumed by `dispatchJob` and forwarded to `IObserver::onJobFailure`.
     * Supplying diagnostic text may allocate; leave it unset on success.
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
     * time while the graph is stable; individual status reads may reflect
     * different instants and do not form a coherent whole-graph snapshot.
     * Allocates one `std::vector` per call; poll at the display frame rate (not
     * tighter than 16 ms) to avoid unnecessary pressure.
     *
     * For single-string "current job" display prefer `IObserver::onJobStart` feeding
     * an atomic pointer -- zero allocation, zero polling.
     */
    struct JobSnapshot {
        std::string_view name;        ///< Stable pointer into the pipeline's node; valid until pipeline is destroyed.
        JobStatus        status;      ///< Relaxed atomic load of the job's current status.
        std::string_view statusText;  ///< Display text set with Job::statusText(); empty if none.
    };
    /** @note Not noexcept: building the vector can throw std::bad_alloc. */
    [[nodiscard]] std::vector<JobSnapshot> snapshot() const;

    /**
     * @brief Display text set for a job with Job::statusText().
     * @param id  A job identifier, as observers receive it.
     * @return The text, or an empty view if none was set or @p id is invalid.
     *         Borrowed from the caller that set it.
     * @note Safe to call from an observer callback during a run.
     */
    [[nodiscard]] auto statusText(JobId id) const noexcept -> std::string_view;

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
     *   DefaultExecutor exec;
     *   pipeline.arm(exec);
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
     * Primary use case: a device transfer queue drainer that must exit cleanly when
     * a client session disconnects.
     */
    [[nodiscard]] Job add_on_demand(
        std::function<std::expected<void, PipelineError>(std::stop_token)> fn);

    /**
     * @brief Dispatch an on-demand job via the armed executor.
     *
     * Requires arm(), a stable graph and a thread-safe executor for concurrent
     * submissions. Do not overlap run(), mutation or destruction. Returns kBusy
     * for a queued/running duplicate or unreaped timeout work; foreign handles
     * return kUnknownJob. Each accepted invocation starts fresh cancellation.
     * Wait for executor completion and reap orphans before retry or teardown.
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

    /** Write a human-readable dependency list to the caller-selected stream. */
    void dump_text(std::ostream& output) const;

private:
    struct Node;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class Job;

    // Add a job whose callable ignores its stop token (not cancellable).
    [[nodiscard]] Job emplacePlain(
        std::function<std::expected<void, PipelineError>(std::stop_token)> fn);

    Node&       node(uint32_t idx);
    const Node& node(uint32_t idx) const;

    // Both public overloads use the same cancellation and execution path.
    [[nodiscard]] auto runImpl(IExecutor& executor, std::stop_token external,
                               IObserver* observer)
        -> std::expected<void, PipelineError>;
};

} // namespace sub0pipeline
