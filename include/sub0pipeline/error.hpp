// include/sub0pipeline/error.hpp
//
// PipelineError — error codes returned by Pipeline operations.
#pragma once

#include <cstdint>

namespace sub0pipeline {

// ── Error types ──────────────────────────────────────────────────────────────

/** Error codes returned by Pipeline operations. */
enum class PipelineError : uint8_t
{
    kTimeout,           ///< Job exceeded its declared timeout.
    kJobFailed,         ///< Job function returned an unexpected error.
    kCyclicDependency,  ///< The DAG contains a cycle.
    kUnknownJob,        ///< Operation on an invalid Job handle.
    kNotArmed,          ///< trigger() called before arm() -- no executor stored.
    kNotOnDemand,       ///< trigger() called on a job not registered via add_on_demand().
    kCancelled,         ///< Job was cancelled externally via Job::cancel() or a stop token.
    kBusy,              ///< Another run or invocation is already active.
    kDeadlineUnavailable, ///< Injected deadline service has no free registration.
};

} // namespace sub0pipeline
