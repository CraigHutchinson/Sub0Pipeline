// include/sub0pipeline/job_group.hpp
//
// JobGroup — a set of parallel Job handles wired as one unit, and parallel().
#pragma once

#include "sub0pipeline/job.hpp"

#include <concepts>
#include <type_traits>
#include <vector>

namespace sub0pipeline {

// ── JobGroup ────────────────────────────────────────────────────────────────

/**
 * @brief A named group of parallel Job handles.
 *
 * Provides .succeed() and .precede() that delegate to every member,
 * allowing a group to be wired as a single unit in dependency expressions.
 * Created via parallel() or DSL operator+.
 */
class JobGroup
{
public:
    JobGroup() = default;

    /** Construct from two jobs. */
    explicit JobGroup(Job first, Job second)
        : jobs_{first, second} {}

    /** Add a job to the group. Returns *this for chaining. */
    JobGroup& add(Job j) { jobs_.push_back(j); return *this; }

    /** Every job in this group runs AFTER @p other. */
    JobGroup& succeed(Job other);

    /** Every job in this group runs AFTER every job in @p other. */
    JobGroup& succeed(JobGroup const& other);

    /** Variadic: every job in this group runs AFTER all listed jobs. */
    template<typename... Jobs>
    JobGroup& succeed(Job first, Jobs... rest)
    {
        succeed(first);
        if constexpr (sizeof...(rest) > 0) succeed(rest...);
        return *this;
    }

    /** Every job in @p other runs AFTER every job in this group. */
    JobGroup& precede(Job other);

    /** Every job in @p other group runs AFTER every job in this group. */
    JobGroup& precede(JobGroup const& other);

    /** Variadic: all listed jobs run AFTER every job in this group. */
    template<typename... Jobs>
    JobGroup& precede(Job first, Jobs... rest)
    {
        precede(first);
        if constexpr (sizeof...(rest) > 0) precede(rest...);
        return *this;
    }

    /** Read-only view of member jobs. */
    [[nodiscard]] const std::vector<Job>& jobs() const noexcept { return jobs_; }

private:
    std::vector<Job> jobs_;
};

/**
 * @brief Create a group of parallel jobs.
 * @example auto io = parallel(display, network, audio);
 */
template<typename... Jobs_t>
    requires (std::same_as<std::remove_cvref_t<Jobs_t>, Job> && ...)
[[nodiscard]] JobGroup parallel(Jobs_t... jobs)
{
    JobGroup g;
    (g.add(jobs), ...);
    return g;
}

} // namespace sub0pipeline
