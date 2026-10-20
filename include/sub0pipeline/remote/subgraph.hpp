// include/sub0pipeline/remote/subgraph.hpp
//
// SubGraphRequest -- jobs of registered kinds and the edges between them, sent
// as one unit -- and SubGraphReply. Link Sub0Pipeline::Remote.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sub0pipeline/error.hpp"
#include "sub0pipeline/job.hpp"
#include "sub0pipeline/remote/payload.hpp"
#include "sub0pipeline/remote/registry.hpp"

namespace sub0pipeline::remote
{

/**
 * A sub-graph to run on a worker: jobs of registered kinds, each with an input
 * payload, and the edges between them. A single remote job is a request of
 * one job. An edge orders two jobs and passes the first one's output to the
 * second. Only outputs marked exported are sent back.
 */
class SubGraphRequest
{
public:
    /** One job of the request. */
    struct JobSpec
    {
        JobKindId kind;            ///< The registered kind to run.
        Payload   input;           ///< The job's own input.
        bool      exported{false}; ///< Whether the reply carries this job's output.
    };

    /** An edge: @p to runs after @p from and receives its output. */
    struct Edge
    {
        uint32_t from{0}; ///< Index of the earlier job.
        uint32_t to{0};   ///< Index of the later job.
    };

    /**
     * Adds a job.
     * @param kind      The registered kind to run.
     * @param input     The job's input.
     * @param exported  Whether the reply carries this job's output.
     * @return The job's index, for edge() and in the reply.
     */
    uint32_t add(JobKindId kind, Payload input = {}, bool exported = false)
    {
        jobs_.push_back({std::move(kind), std::move(input), exported});
        return static_cast<uint32_t>(jobs_.size() - 1U);
    }

    /**
     * Adds an edge from job @p from to job @p to.
     * @param from  Index of the earlier job.
     * @param to    Index of the later job.
     * @return false, with nothing added, if either index is out of range or
     *         they are equal.
     */
    bool edge(uint32_t from, uint32_t to)
    {
        if (from >= jobs_.size() || to >= jobs_.size() || from == to) return false;
        edges_.push_back({from, to});
        return true;
    }

    /**
     * Marks job @p job's output to be sent back. No effect if out of range.
     * @param job  The job's index.
     */
    void exportOutput(uint32_t job) noexcept
    {
        if (job < jobs_.size()) jobs_[job].exported = true;
    }

    /**
     * Returns the jobs in index order.
     * @return The jobs.
     */
    [[nodiscard]] const std::vector<JobSpec>& jobs() const noexcept { return jobs_; }

    /**
     * Returns the edges in the order they were added.
     * @return The edges.
     */
    [[nodiscard]] const std::vector<Edge>& edges() const noexcept { return edges_; }

private:
    std::vector<JobSpec> jobs_;
    std::vector<Edge>    edges_;
};

/** The outcome of one job of a request. */
struct JobResult
{
    JobStatus     status{JobStatus::kPending};       ///< kDone, kFailed, kSkipped, kCancelled or kTimedOut.
    PipelineError error{PipelineError::kJobFailed};  ///< The error, when the job did not finish kDone.
    std::string   message;                           ///< Diagnostic text from JobContext::setError().
    Payload       output;                            ///< Exported output of a kDone job; empty otherwise.
};

/** The reply to a request: one result per job, in request order. */
struct SubGraphReply
{
    std::vector<JobResult> results; ///< One per job of the request.

    /**
     * Finds the failure that the remote job reports: the lowest job index
     * that failed, was cancelled or timed out. Skipped jobs are not failures.
     * Index order keeps the result independent of worker scheduling.
     * @return That index, or nothing if no job failed.
     */
    [[nodiscard]] std::optional<uint32_t> firstFailure() const noexcept
    {
        for (uint32_t i = 0U; i < results.size(); ++i)
        {
            const auto status = results[i].status;
            if (status == JobStatus::kFailed || status == JobStatus::kCancelled
                || status == JobStatus::kTimedOut)
            {
                return i;
            }
        }
        return std::nullopt;
    }
};

} // namespace sub0pipeline::remote
