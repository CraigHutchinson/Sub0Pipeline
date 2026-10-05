// include/sub0pipeline/executor_factory.hpp
//
// Factory functions for the bundled platform executors. Each is defined in its
// platform library (Sub0Pipeline::Desktop / ::Headless / ::Priority, or the
// esp32p4 ESP-IDF component); link the one you call.
#pragma once

#include <sub0pipeline/executor.hpp>

#include <functional>
#include <memory>

namespace sub0pipeline {

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
 * Typical device usage: blocking fetches (.priority(10)) preempt prefetch
 * hints (.priority(5)) when the thread pool is under load.
 *
 * @param threadCount    Worker thread count. 0 = `hardware_concurrency()`.
 * @param onThreadStart  Optional hook invoked once on each worker thread,
 *                        before it services any job. Runs on the worker
 *                        thread itself, not the calling thread. Intended for
 *                        one-time per-thread setup that would otherwise
 *                        lazy-init on the first dispatched job and land on
 *                        that job's critical path -- e.g. naming the thread
 *                        for a debugger/profiler, pinning affinity, or
 *                        eagerly touching thread_local state a subsystem
 *                        would otherwise initialise on first use.
 *
 * Defined in `platform/priority/`.
 */
std::unique_ptr<IExecutor> makePriorityExecutor(
    unsigned int threadCount = 0, std::function<void()> onThreadStart = nullptr);

/// Returns a `FreeRtosExecutor` (one FreeRTOS task per job). Defined in
/// `platform/esp32p4/`, which builds only as an ESP-IDF component.
std::unique_ptr<IExecutor> makeFreeRtosExecutor();

} // namespace sub0pipeline
