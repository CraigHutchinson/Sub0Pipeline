// include/sub0pipeline/remote/registry.hpp
//
// JobRegistry -- the job kinds a worker can run, by name and version -- and
// the JobContext a kind's function receives. Link Sub0Pipeline::Remote.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sub0pipeline/error.hpp"
#include "sub0pipeline/remote/payload.hpp"

namespace sub0pipeline::remote
{

class IPayloadStrategy;

/** Names one job kind. A request names kinds; no code travels. */
struct JobKindId
{
    std::string name;       ///< At most 65,535 bytes.
    uint32_t    version{0}; ///< Bump when the kind's inputs, outputs or behaviour change.

    /** Orders by name, then version. */
    [[nodiscard]] auto operator<=>(const JobKindId&) const = default;
};

/**
 * What a job kind's function sees while it runs on a worker.
 *
 * Valid only during that call. Predecessor outputs are those of the jobs with
 * an edge into this one, in the order the edges were added to the request.
 */
class JobContext
{
public:
    /**
     * Built by the worker for each job it runs.
     * @param input         The job's own input payload.
     * @param predecessors  Outputs of the jobs with an edge into this one.
     * @param output        Receives the job's output.
     * @param error         Receives diagnostic text for a failure.
     * @param store         The worker's payload store, or nullptr.
     * @param stop          Stops when the request is cancelled.
     * @param strategy      The connection's payload strategy, for allocateOutput(); may be nullptr.
     */
    JobContext(const Payload& input, std::span<const Payload* const> predecessors, Payload& output,
               std::string& error, IPayloadStore* store, std::stop_token stop,
               IPayloadStrategy* strategy = nullptr) noexcept
        : input_{input}, predecessors_{predecessors}, output_{output}, error_{error}, store_{store},
          stop_{std::move(stop)}, strategy_{strategy} {}

    /**
     * Returns the job's own input.
     * @return The payload the request gave this job.
     */
    [[nodiscard]] const Payload& input() const noexcept { return input_; }

    /**
     * Reports how many jobs have an edge into this one.
     * @return The number of predecessor outputs.
     */
    [[nodiscard]] std::size_t predecessorCount() const noexcept { return predecessors_.size(); }

    /**
     * Returns one predecessor's output.
     * @param index  Position among this job's incoming edges, in request order.
     * @return The output; empty if that predecessor produced none.
     */
    [[nodiscard]] const Payload& predecessor(std::size_t index) const noexcept { return *predecessors_[index]; }

    /**
     * Sets the job's output. An inline output larger than the connection's
     * inline limit is moved into the store and returned as a handle.
     * @param output  The output payload.
     */
    void setOutput(Payload output) noexcept { output_ = std::move(output); }

    /**
     * Allocates the job's output for writing in place and makes it the output.
     * With a shared-memory strategy the buffer lies in the mapped region, so
     * the reply needs no copy; otherwise it is ordinary memory.
     * @param size  The output's size in bytes.
     * @return The writable bytes, valid until the request's reply is sent.
     */
    [[nodiscard]] std::span<std::byte> allocateOutput(std::size_t size);

    /**
     * Sets diagnostic text reported with a failure.
     * @param message  The text. Copied.
     */
    void setError(std::string_view message) { error_.assign(message); }

    /**
     * Returns the worker's payload store.
     * @return The store, or nullptr if the worker has none.
     */
    [[nodiscard]] IPayloadStore* store() const noexcept { return store_; }

    /**
     * Returns a token that stops when the coordinator cancels the request.
     * @return The token.
     */
    [[nodiscard]] const std::stop_token& stopToken() const noexcept { return stop_; }

private:
    const Payload&                 input_;
    std::span<const Payload* const> predecessors_;
    Payload&                       output_;
    std::string&                   error_;
    IPayloadStore*                 store_; // non-owning
    std::stop_token                stop_;
    IPayloadStrategy*              strategy_; // non-owning
};

/** A job kind's function. Must be thread-safe and must not throw. */
using JobKindFn = std::function<std::expected<void, PipelineError>(JobContext&)>;

/**
 * Returns the fingerprint of a set of job kinds: FNV-1a 64 over the kinds
 * sorted by name then version, each as a u16 length, the name bytes and a u32
 * version, little-endian. The order of @p kinds does not matter.
 * @param kinds  The kinds.
 * @return The fingerprint.
 */
[[nodiscard]] uint64_t registryFingerprint(std::span<const JobKindId> kinds);

/**
 * The job kinds a worker can run. Build it before serving; it is read-only
 * while a WorkerHost uses it.
 */
class JobRegistry
{
public:
    /**
     * Registers @p fn as job kind @p kind.
     * @param kind  The kind's name and version.
     * @param fn    The function to run for it.
     * @return false if that name and version are already registered or @p fn
     *         is empty; nothing changes then.
     */
    bool add(JobKindId kind, JobKindFn fn);

    /**
     * Finds the function for a kind.
     * @param name     The kind's name.
     * @param version  The kind's version.
     * @return The function, or nullptr if it is not registered.
     */
    [[nodiscard]] const JobKindFn* find(std::string_view name, uint32_t version) const noexcept;

    /**
     * Lists the registered kinds.
     * @return The kinds, sorted by name then version.
     */
    [[nodiscard]] std::vector<JobKindId> kinds() const;

    /**
     * Returns registryFingerprint() of the registered kinds.
     * @return The fingerprint a coordinator must expect to connect.
     */
    [[nodiscard]] uint64_t fingerprint() const;

private:
    std::map<JobKindId, JobKindFn> kinds_;
};

} // namespace sub0pipeline::remote
