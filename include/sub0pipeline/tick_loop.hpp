// include/sub0pipeline/tick_loop.hpp
//
// TickLoop — runs recurring jobs at their intervals until asked to stop.
#pragma once

#include <chrono>
#include <functional>
#include <stop_token>
#include <vector>

namespace sub0pipeline {

/** Describes one recurring job for a TickLoop. */
struct TickJob
{
    std::chrono::milliseconds interval;  ///< Minimum time between calls; 0 calls it on every pass.
    std::function<void()>     fn;        ///< Called on the thread that runs the loop. Must not throw.
};

/**
 * Calls recurring jobs at their intervals in a steady-state loop.
 *
 * Typical use is the phase after start-up: run a Pipeline once to bring the
 * system up, then hand the thread to a TickLoop for periodic work.
 * @code
 *   TickLoop ticks;
 *   ticks.add({std::chrono::milliseconds{100}, pollSensors});
 *   std::jthread worker{[&](std::stop_token stop) { ticks.run(stop); }};
 * @endcode
 *
 * It is independent of Pipeline: it owns only its jobs, and shares no state
 * with any graph. Not thread-safe: add every job before run() starts, and call
 * run() from one thread at a time. The loop and everything its jobs borrow
 * must outlive run().
 */
class TickLoop
{
public:
    /**
     * Add a recurring job.
     * @param tick  The job and its interval. Its first call happens on the
     *              first pass of run().
     * @note Not while run() is executing.
     */
    void add(TickJob tick);

    /**
     * Call each job whenever its interval has elapsed, until @p stop is requested.
     *
     * A pass visits every job in the order added, then yields for one
     * millisecond (one RTOS tick on FreeRTOS). Stop is checked between passes:
     * a job that is running is allowed to finish, and the yield is not
     * interruptible, so return can lag the request by one job plus one yield.
     * Intervals are measured from each run() call; a job that overruns is not
     * called extra times to catch up.
     *
     * @param stop  Request stop on this to make run() return. A token that can
     *              never be stopped makes run() loop forever.
     */
    void run(std::stop_token stop);

private:
    std::vector<TickJob> ticks_;
};

} // namespace sub0pipeline
