// include/sub0pipeline/remote/worker_host.hpp
//
// WorkerHost -- the worker's end of a connection: runs each received request
// as its own Pipeline and sends the reply. Link Sub0Pipeline::Remote.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <unordered_map>

#include "sub0pipeline/executor/executor.hpp"
#include "sub0pipeline/remote/payload.hpp"
#include "sub0pipeline/remote/registry.hpp"
#include "sub0pipeline/remote/transport.hpp"

namespace sub0pipeline::remote
{

/**
 * The worker's end of a connection to one coordinator.
 *
 * Answers the handshake (refusing a different protocol major version or a
 * registry fingerprint other than the coordinator expects), then runs each
 * request as a fresh Pipeline on a ScopedExecutor over Options::executor, so
 * the jobs of one request run in parallel and one request never waits for
 * another's jobs. Up to Options::maxConcurrentRequests requests run at once;
 * the handshake advertises that limit, and a request beyond it is answered
 * at once with every job failed kBusy.
 *
 * The library never starts processes: the consumer starts the worker process
 * and gives each side its transport.
 */
class WorkerHost final : private IFrameSink
{
public:
    /** Settings. */
    struct Options
    {
        IExecutor*     executor{nullptr};           ///< Runs the jobs; nullptr runs each request inline on its runner thread.
        uint32_t       maxConcurrentRequests{4U};   ///< Requests run at once, one runner thread each.
        uint32_t       maxInlineBytes{64U * 1024U}; ///< Largest inline payload either way; larger outputs go to the store.
        IPayloadStore* store{nullptr};              ///< Resolves handles for jobs and stores large outputs; optional.
    };

    /**
     * Prepares a host. Nothing happens until serve().
     * @param registry   The kinds this worker runs. Borrowed and read-only while serving.
     * @param transport  The connection to the coordinator. Borrowed; must outlive the host.
     * @param options    Settings; the executor and store are borrowed.
     */
    WorkerHost(const JobRegistry& registry, ITransport& transport, Options options);

    /** Destroy only after serve() has returned. */
    ~WorkerHost() override;

    WorkerHost(const WorkerHost&)            = delete;
    WorkerHost& operator=(const WorkerHost&) = delete;

    /**
     * Serves the connection until it closes or @p stop is requested, then
     * cancels running requests and joins them.
     * @param stop  Stops serving; the transport is then closed.
     * @note Call once.
     */
    void serve(std::stop_token stop);

private:
    struct Work;

    void onFrame(std::span<const std::byte> frame) override;
    void onClosed(TransportError reason) noexcept override;

    void runner(std::stop_token stop);
    void run(Work& work);

    const JobRegistry&                               registry_;
    ITransport&                                      transport_;
    Options                                          options_;
    std::mutex                                       mtx_;
    std::condition_variable_any                      changed_;
    bool                                             handshaken_{false};
    bool                                             closed_{false};
    uint32_t                                         admitted_{0U}; ///< Queued plus running requests.
    uint32_t                                         inlineLimit_{0U}; ///< Smaller of both sides' inline limits.
    std::deque<std::shared_ptr<Work>>                queue_;
    std::unordered_map<uint64_t, std::shared_ptr<Work>> active_;
};

} // namespace sub0pipeline::remote
