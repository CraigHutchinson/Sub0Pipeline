// include/sub0pipeline/observer.hpp
//
// IObserver — opt-in run/job/dependency event hooks, and the RunId that groups them.
#pragma once

#include <cstdint>
#include <string_view>

#include "sub0pipeline/dependency_range.hpp"
#include "sub0pipeline/error.hpp"
#include "sub0pipeline/job.hpp"

namespace sub0pipeline
{

/// An observer-defined identifier for one observed execution or trigger.
using RunId = uint64_t;

// ── Observer interface ────────────────────────────────────────────────────────

/**
 * Pluggable observer for profiling and progress tracking.
 *
 * Attach via Pipeline::run() or Pipeline::arm() to receive identity-aware
 * callbacks. Events are not recorded unless a caller supplies an observer.
 * Callbacks are synchronous and may overlap on parallel executors. Implementations
 * must be thread-safe, bounded and non-throwing; callback exceptions are not
 * converted to PipelineError. Keep the observer alive until executor callbacks join.
 * Names and failure messages are borrowed; copy them during the callback to retain
 * them. Do not mutate, move or destroy the graph from a callback.
 */
class IObserver
{
public:
    virtual ~IObserver() = default;

    /**
     * Start one observed run or on-demand trigger.
     *
     * Return an identifier that the following job and dependency callbacks
     * can use to group events. The default 0 is suitable when the observer
     * does not need run correlation.
     * Called on the run() caller's thread, or an executor worker for trigger().
     *
     * @return An identifier for the run, passed to the following callbacks.
     */
    virtual RunId onRunStart() { return 0; }

    /**
     * Called just before a job starts executing. Keep implementations fast.
     * @param runId    Observer-defined identifier for this run or trigger.
     * @param jobId    Stable node index within the Pipeline.
     * @param jobName  The job's name.
     */
    virtual void onJobStart([[maybe_unused]] RunId runId,
                            [[maybe_unused]] JobId jobId,
                            [[maybe_unused]] std::string_view jobName) {}

    /**
     * Called when a job completes, including skipped jobs with no start event.
     * @param runId    Observer-defined identifier for this run or trigger.
     * @param jobId    Stable node index within the Pipeline.
     * @param jobName  The job's name.
     * @param status   Final status (kDone, kFailed, kSkipped, kTimedOut, kCancelled).
     * @param progress Fraction of total jobs completed (0.0–1.0).
     * Concurrent callbacks may deliver progress values out of order.
     */
    virtual void onJobFinish([[maybe_unused]] RunId runId,
                             [[maybe_unused]] JobId jobId,
                             [[maybe_unused]] std::string_view jobName,
                             [[maybe_unused]] JobStatus status,
                             [[maybe_unused]] float progress) {}

    /**
     * Called for non-optional failures, cancellation and submission rejection,
     * including cancellation and rejection on optional jobs during run().
     *
     * Separate from onJobFinish so callers only opt into failure detail when they
     * need it. The observer vtable dispatch is gated by `if (observer)`.
     *
     * @param runId    Observer-defined identifier for this run or trigger.
     * @param jobId    Stable node index within the Pipeline.
     * @param jobName  Name of the failed job.
     * @param error    The PipelineError code.
     * @param message  Diagnostic string set by the job via
     *                 Pipeline::setCurrentJobError() -- empty if the job
     *                 did not provide context or submission was rejected. Valid only during
     *                 this callback; supplying diagnostic text may allocate.
     */
    virtual void onJobFailure([[maybe_unused]] RunId runId,
                              [[maybe_unused]] JobId jobId,
                              [[maybe_unused]] std::string_view jobName,
                              [[maybe_unused]] PipelineError error,
                              [[maybe_unused]] std::string_view message) noexcept {}

    /**
     * Called once per completed node that has successors, rather than once per
     * edge. The range is non-owning and valid only during this callback.
     * @param runId       Observer-defined identifier for this run or trigger.
     * @param from        Stable node index of the completed job.
     * @param fromName    The completed job's name.
     * @param successors  The completed job's successors.
     */
    virtual void onDependenciesResolved([[maybe_unused]] RunId runId,
                                        [[maybe_unused]] JobId from,
                                        [[maybe_unused]] std::string_view fromName,
                                        [[maybe_unused]] DependencyRange successors) {}
};

} // namespace sub0pipeline
