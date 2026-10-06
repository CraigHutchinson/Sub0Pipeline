// include/sub0pipeline/job_group.hpp
//
// JobGroup — a set of parallel Job handles wired as one unit, and parallel().
#pragma once

#include <concepts>
#include <type_traits>
#include <vector>

#include "sub0pipeline/job.hpp"

namespace sub0pipeline
{

// ── JobGroup ────────────────────────────────────────────────────────────────

/**
 * Groups parallel Job handles that are wired as a unit.
 *
 * Provides .succeed() and .precede() that delegate to every member,
 * allowing a group to be wired as a single unit in dependency expressions.
 * Created via parallel() or DSL operator+.
 */
class JobGroup
{
public:
    JobGroup() = default;

    /**
     * Construct from two jobs.
     * @param first   The first member.
     * @param second  The second member.
     */
    explicit JobGroup(Job first, Job second)
        : jobs_{first, second} {}

    /**
     * Add a job to the group.
     * @param j  The job to add.
     * @return *this for chaining.
     */
    JobGroup& add(Job j) { jobs_.push_back(j); return *this; }

    /**
     * Every job in this group runs AFTER @p other.
     * @param other  The predecessor job.
     * @return *this for chaining.
     */
    JobGroup& succeed(Job other);

    /**
     * Every job in this group runs AFTER every job in @p other.
     * @param other  The predecessor group.
     * @return *this for chaining.
     */
    JobGroup& succeed(JobGroup const& other);

    /**
     * Every job in this group runs AFTER all listed jobs.
     * @tparam Jobs  Further Job handles.
     * @param first  The first predecessor.
     * @param rest   The remaining predecessors.
     * @return *this for chaining.
     */
    template<typename... Jobs>
    JobGroup& succeed(Job first, Jobs... rest)
    {
        succeed(first);
        if constexpr (sizeof...(rest) > 0) succeed(rest...);
        return *this;
    }

    /**
     * Every job in @p other runs AFTER every job in this group.
     * @param other  The successor job.
     * @return *this for chaining.
     */
    JobGroup& precede(Job other);

    /**
     * Every job in @p other group runs AFTER every job in this group.
     * @param other  The successor group.
     * @return *this for chaining.
     */
    JobGroup& precede(JobGroup const& other);

    /**
     * All listed jobs run AFTER every job in this group.
     * @tparam Jobs  Further Job handles.
     * @param first  The first successor.
     * @param rest   The remaining successors.
     * @return *this for chaining.
     */
    template<typename... Jobs>
    JobGroup& precede(Job first, Jobs... rest)
    {
        precede(first);
        if constexpr (sizeof...(rest) > 0) precede(rest...);
        return *this;
    }

    /**
     * Gives read-only access to the member jobs.
     * @return The members, in the order they were added.
     */
    [[nodiscard]] const std::vector<Job>& jobs() const noexcept { return jobs_; }

private:
    std::vector<Job> jobs_;
};

/**
 * Create a group of parallel jobs.
 * @example auto io = parallel(display, network, audio);
 * @tparam Jobs_t  Job handle types; each must be Job.
 * @param jobs     The jobs to group.
 * @return A JobGroup holding every listed job.
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
