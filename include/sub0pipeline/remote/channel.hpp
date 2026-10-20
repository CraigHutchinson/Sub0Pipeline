// include/sub0pipeline/remote/channel.hpp
//
// RemoteChannel -- the coordinator's end of a connection to one worker. Each
// request appears in a Pipeline as one deferred job. Link Sub0Pipeline::Remote.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include <unordered_map>

#include "sub0pipeline/completion.hpp"
#include "sub0pipeline/pipeline.hpp"
#include "sub0pipeline/remote/strategy.hpp"
#include "sub0pipeline/remote/subgraph.hpp"
#include "sub0pipeline/remote/transport.hpp"
#include "sub0pipeline/remote/wire.hpp"

namespace sub0pipeline::remote
{

/** Why RemoteChannel::connect() failed. */
enum class ConnectError : uint8_t
{
    kVersionMismatch,     ///< The worker speaks another protocol major version.
    kFingerprintMismatch, ///< The worker's registry differs from the expected one.
    kTransportClosed,     ///< The transport closed before the handshake finished.
    kTimeout,             ///< No answer within Options::handshakeTimeout.
    kProtocol,            ///< The worker answered with something other than a handshake frame.
};

/** Called with a request's reply before its job completes; not called if the job was cancelled. */
using ReplyHandler = std::function<void(const SubGraphReply&)>;

/**
 * The coordinator's end of a connection to one worker.
 *
 * emplace() adds a deferred job that sends a SubGraphRequest when it starts
 * and completes when the reply arrives. It holds no executor thread while
 * waiting. The job fails with the lowest-index failure in the reply
 * (SubGraphReply::firstFailure()), kUnavailable when the connection is lost,
 * and kBusy when the channel is full.
 *
 * Back-pressure: at most the negotiated in-flight limit (the smaller of
 * Options::maxInFlight and the worker's) is outstanding. Further requests wait
 * in a queue of Options::queueCapacity, holding only their Completion; when
 * that is full too they fail at once with kBusy. freeCapacity() reports the
 * room left.
 *
 * Cancelling a remote job finishes it at once (deferred-job rules) and sends
 * the worker a cancel; the late reply is dropped.
 *
 * @note All members are thread-safe. The transport must outlive the channel;
 *       destroying the channel closes the transport and fails outstanding
 *       requests with kUnavailable.
 */
class RemoteChannel final : private IFrameSink
{
public:
    /** Settings; the limits are negotiated down to the worker's. */
    struct Options
    {
        uint64_t                  expectedFingerprint{0U};            ///< registryFingerprint() of the kinds this side uses.
        uint32_t                  maxInFlight{8U};                    ///< Requests outstanding at once.
        uint32_t                  queueCapacity{0U};                  ///< Requests waiting for a slot; 0 fails fast with kBusy.
        uint32_t                  maxInlineBytes{64U * 1024U};        ///< Largest inline payload either way.
        std::chrono::milliseconds handshakeTimeout{std::chrono::seconds{5}}; ///< How long connect() waits.
        /// Payload strategies to offer, most preferred first; streamStrategy() is
        /// always appended, so a worker that supports none of these still connects.
        std::vector<PayloadStrategyFactory> strategies{};
    };

    /**
     * Prepares a channel over @p transport. Nothing is sent until connect().
     * @param transport  The connection to the worker. Borrowed; must outlive the channel.
     * @param options    Settings.
     */
    RemoteChannel(ITransport& transport, Options options);

    /** Closes the transport, fails outstanding requests with kUnavailable, waits for the close. */
    ~RemoteChannel() override;

    RemoteChannel(const RemoteChannel&)            = delete;
    RemoteChannel& operator=(const RemoteChannel&) = delete;

    /**
     * Starts the transport and performs the handshake.
     * @return Empty once connected, or why the worker could not be used. A
     *         failed channel stays closed; create a new one to retry.
     * @note Call once, before submitting.
     */
    [[nodiscard]] std::expected<void, ConnectError> connect();

    /**
     * Adds a deferred job to @p pipeline that runs @p request on the worker.
     * @param pipeline  The coordinator's graph.
     * @param request   The sub-graph; copied into the job, which re-sends it on every run.
     * @param onReply   Optional; receives the reply before the job completes.
     * @return The job, for naming and dependencies.
     */
    [[nodiscard]] Job emplace(Pipeline& pipeline, SubGraphRequest request, ReplyHandler onReply = {});

    /**
     * Sends @p request and completes @p done with its outcome. emplace() uses
     * this; call it directly from an existing deferred job.
     * @param request  The sub-graph.
     * @param done     The job to complete.
     * @param onReply  Optional; receives the reply before @p done completes.
     */
    void submit(const SubGraphRequest& request, Completion done, ReplyHandler onReply = {});

    /**
     * Reports how many more requests can be submitted without kBusy.
     * @return Free in-flight slots plus free queue slots; 0 when not connected.
     */
    [[nodiscard]] uint32_t freeCapacity() const;

    /**
     * Allocates an input payload to write in place before submitting it. With
     * a shared-memory strategy the bytes lie in the mapped region and are
     * never copied by the library.
     * @param size  Bytes to allocate.
     * @return The payload and its writable bytes; ordinary memory before connect().
     */
    [[nodiscard]] WritablePayload allocatePayload(std::size_t size);

    /**
     * Reports the payload strategy the handshake chose.
     * @return Its name, or empty before connecting or with a minor-0 worker.
     */
    [[nodiscard]] std::string payloadStrategy() const;

    /**
     * Returns the worker's handshake values.
     * @return The values; meaningful after a successful connect().
     */
    [[nodiscard]] Hello peer() const;

private:
    struct Pending;
    struct Queued;
    enum class State : uint8_t { kIdle, kHandshaking, kConnected, kClosed };

    void onFrame(std::span<const std::byte> frame) override;
    void onFrameBuffer(std::shared_ptr<const void> owner, std::span<const std::byte> frame) override;
    void onClosed(TransportError reason) noexcept override;

    void send(std::shared_ptr<const SubGraphRequest> request, Completion done, ReplyHandler onReply);
    void failPending(uint64_t id, PipelineError error, const std::string& message);
    std::unique_ptr<Queued> takeQueued(); // with mtx_ held
    void launch(std::unique_ptr<Queued> next);
    void sendCancel(uint64_t id) noexcept;
    void handleReply(std::span<const std::byte> frame);
    bool selectStrategy(const Hello& ack); // with mtx_ held

    ITransport&                                         transport_;
    Options                                             options_;
    mutable std::mutex                                  mtx_;
    std::condition_variable                             changed_;
    State                                               state_{State::kIdle};
    bool                                                started_{false};
    bool                                                closed_{false};
    std::optional<ConnectError>                         rejected_;
    Hello                                               peer_;
    uint32_t                                            inFlightLimit_{0U};
    uint32_t                                            inlineLimit_{0U};
    uint64_t                                            nextId_{1U};
    std::unordered_map<uint64_t, std::unique_ptr<Pending>> pending_;
    std::vector<std::unique_ptr<IPayloadStrategy>>      candidates_; ///< Offered, until the handshake chooses.
    std::unique_ptr<IPayloadStrategy>                   strategy_;   ///< Chosen at the handshake; null for none.
    std::unique_ptr<IPayloadSender>                     sender_;
    std::deque<std::unique_ptr<Queued>>                 queued_;
};

} // namespace sub0pipeline::remote
