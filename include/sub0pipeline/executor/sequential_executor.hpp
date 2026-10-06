// include/sub0pipeline/executor/sequential_executor.hpp
//
// SequentialExecutor — runs jobs on the calling thread, no threads.
// Header-only: nothing to link beyond the core library.
#pragma once

#include <sub0pipeline/executor/executor.hpp>

#include <cstdint>
#include <functional>
#include <string_view>

namespace sub0pipeline {

/**
 * @brief Executor that runs every job on the calling thread.
 *
 * No threads and no state, so execution order is deterministic: ideal for unit
 * tests and for targets without `std::thread`. Because it reports
 * runs_inline(), Pipeline::run() calls ready jobs from a loop in the order they
 * become ready, and stack depth does not grow with the length of a dependency
 * chain. Core affinity, priority and stack hints are ignored.
 *
 * Stateless and trivially constructible: create one wherever it is needed.
 * Any number of threads may use one instance; each runs its own jobs.
 */
class SequentialExecutor final : public IExecutor
{
public:
    /** Runs @p fn, then @p onComplete, before returning. */
    void dispatch(
        std::string_view              /*name*/,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           /*coreAffinity*/,
        uint8_t                       /*priority*/,
        uint32_t                      /*stackBytes*/ = 4096U) override
    {
        fn();
        if (onComplete) onComplete();
    }

    /** Nothing to wait for: every job has finished by the time dispatch() returns. */
    void wait_all() override {}

    /** @return 1. */
    [[nodiscard]] int concurrency() const noexcept override { return 1; }

    /** @return true. */
    [[nodiscard]] bool runs_inline() const noexcept override { return true; }
};

} // namespace sub0pipeline
