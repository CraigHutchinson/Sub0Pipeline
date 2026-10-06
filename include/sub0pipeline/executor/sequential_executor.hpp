// include/sub0pipeline/executor/sequential_executor.hpp
//
// SequentialExecutor — runs jobs on the calling thread, no threads.
// Header-only: nothing to link beyond the core library.
#pragma once

#include "sub0pipeline/executor/executor.hpp"

#include <cstdint>
#include <functional>
#include <string_view>

namespace sub0pipeline
{

/**
 * Runs every job on the calling thread.
 *
 * No threads and no state, so execution order is deterministic: ideal for unit
 * tests and for targets without `std::thread`. Because it reports
 * runsInline(), Pipeline::run() calls ready jobs from a loop in the order they
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
        uint32_t                      /*stackBytes*/) override
    {
        fn();
        if (onComplete) onComplete();
    }

    /** Nothing to wait for: every job has finished by the time dispatch() returns. */
    void waitAll() override {}

    /** Returns 1: jobs never run in parallel. */
    [[nodiscard]] int concurrency() const noexcept override { return 1; }

    /** Returns true: dispatch() runs each job before it returns. */
    [[nodiscard]] bool runsInline() const noexcept override { return true; }
};

} // namespace sub0pipeline
