// include/sub0pipeline/tick_job.hpp
//
// TickJob — recurring task for the Pipeline tick event loop.
#pragma once

#include <chrono>
#include <functional>
#include <string_view>

namespace sub0pipeline {

// ── Tick job ──────────────────────────────────────────────────────────────────

/** Recurring task registered for the tick event loop. */
struct TickJob
{
    std::string_view          name;      ///< Human-readable label.
    std::chrono::milliseconds interval;  ///< Minimum period (0 = every iteration).
    std::function<void()>     fn;        ///< Function to call each interval.
};

} // namespace sub0pipeline
