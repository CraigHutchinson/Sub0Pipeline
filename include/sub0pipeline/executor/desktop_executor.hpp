// include/sub0pipeline/executor/desktop_executor.hpp
//
// DesktopExecutor — one native thread per job.
// Link Sub0Pipeline::Desktop.
#pragma once

#include <sub0pipeline/executor/executor.hpp>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace sub0pipeline {

/**
 * @brief Executor that starts one `std::thread` per dispatched job.
 *
 * Real parallelism with no pool to size, for desktop simulation and integration
 * tests. Thread creation costs tens of microseconds per job, so prefer
 * PriorityExecutor for throughput. Core affinity, priority and stack hints are
 * ignored.
 *
 * Owns its threads: wait_all() and the destructor join every job dispatched so
 * far. Construct it wherever suits the caller (stack, member, static); it is
 * neither copyable nor movable. dispatch() is thread-safe. Do not call
 * wait_all() from inside a job running on this executor; use ScopedExecutor
 * for nested runs.
 */
class DesktopExecutor final : public IExecutor
{
public:
    DesktopExecutor() = default;

    /** Joins every dispatched job. */
    ~DesktopExecutor() override;

    DesktopExecutor(const DesktopExecutor&)            = delete;
    DesktopExecutor& operator=(const DesktopExecutor&) = delete;

    void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           coreAffinity,
        uint8_t                       priority,
        uint32_t                      stackBytes = 4096U) override;

    void wait_all() override;

    /** @return `std::thread::hardware_concurrency()`. */
    [[nodiscard]] int concurrency() const noexcept override;

private:
    std::mutex                mtx_;      ///< Guards threads_ and inFlight_.
    std::condition_variable   idle_;     ///< Signalled when inFlight_ reaches zero.
    std::vector<std::thread>  threads_;  ///< Started since the last wait_all() join.
    uint32_t                  inFlight_{0U};
};

} // namespace sub0pipeline
