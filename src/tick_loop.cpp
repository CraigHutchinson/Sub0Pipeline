// src/tick_loop.cpp
//
// TickLoop — recurring jobs at fixed minimum intervals.
//
// GCC 15 + ESP-IDF defines _GLIBCXX_HAVE_POSIX_SEMAPHORE which causes
// <functional> → <semaphore> → <semaphore.h> include chain. newlib has
// no POSIX semaphores, so break the chain before any STL includes.
#ifdef _GLIBCXX_HAVE_POSIX_SEMAPHORE
#undef _GLIBCXX_HAVE_POSIX_SEMAPHORE
#endif
#ifdef _GLIBCXX_USE_POSIX_SEMAPHORE
#undef _GLIBCXX_USE_POSIX_SEMAPHORE
#endif

#include <chrono>
#include <cstddef>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "sub0pipeline/tick_loop.hpp"

#if __has_include(<freertos/FreeRTOS.h>)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace sub0pipeline
{

namespace
{
// Yield between passes: one RTOS tick, or 1 ms elsewhere.
void yieldBetweenPasses()
{
#if __has_include(<freertos/FreeRTOS.h>)
    vTaskDelay(1);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
#endif
}
} // namespace

void TickLoop::add(TickJob tick)
{
    ticks_.push_back(std::move(tick));
}

void TickLoop::run(std::stop_token stop)
{
    using Clock = std::chrono::steady_clock;
    // Default time points sit at the clock's epoch, so every job is due on the first pass.
    std::vector<Clock::time_point> lastRun(ticks_.size());

    while (!stop.stop_requested())
    {
        const auto now = Clock::now();

        for (std::size_t i = 0U; i < ticks_.size(); ++i)
        {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRun[i]);
            if (elapsed >= ticks_[i].interval)
            {
                ticks_[i].fn();
                lastRun[i] = now;
            }
        }

        if (stop.stop_requested()) break;

        yieldBetweenPasses();
    }
}

} // namespace sub0pipeline
