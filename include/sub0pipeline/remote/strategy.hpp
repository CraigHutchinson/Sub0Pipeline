// include/sub0pipeline/remote/strategy.hpp
//
// IPayloadStrategy -- how payload bytes move between coordinator and worker,
// chosen per connection independently of the transport -- and the shipped
// streaming and store strategies. Link Sub0Pipeline::Remote.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "sub0pipeline/error.hpp"
#include "sub0pipeline/remote/payload.hpp"
#include "sub0pipeline/remote/transport.hpp"

namespace sub0pipeline::remote
{

/** The body of a payload frame, and what keeps its bytes alive. */
struct FrameView
{
    std::span<const std::byte>  body;  ///< The strategy-defined body, after the frame header.
    std::shared_ptr<const void> owner; ///< Keeps @ref body alive; a strategy may retain it to avoid a copy.
};

/** Sends a strategy's own frames on the connection's transport. Provided by the library. */
class IPayloadSender
{
public:
    virtual ~IPayloadSender() = default;

    /**
     * Sends one payload frame whose body is @p parts, in order.
     * @param parts  The body; the library adds the frame header. Copied before return.
     * @return Empty on success, or why it could not be sent.
     * @note Thread-safe. Frames sent before a control frame arrive before it.
     */
    [[nodiscard]] virtual std::expected<void, TransportError> sendPayloadFrame(
        std::span<const std::span<const std::byte>> parts) = 0;
};

/**
 * Moves payload bytes for one connection. Each side of each connection gets
 * its own instance from a PayloadStrategyFactory.
 *
 * Payloads at or below the connection's inline limit travel in the control
 * frame and never reach a strategy. Larger inline payloads are passed to
 * send(), whose returned reference the control frame carries; the peer's
 * strategy turns it back into a payload with receive().
 *
 * Threading: send() is called from any thread that submits or replies, and
 * receive(), onPayloadFrame(), endScope() and onClosed() from the transport's
 * thread; implementations synchronise their own state. A payload frame sent
 * during send() arrives at the peer before the control frame that references it.
 */
class IPayloadStrategy
{
public:
    virtual ~IPayloadStrategy() = default;

    /**
     * Returns the name advertised in the handshake; both sides must use the same.
     * @return The name, at most 65,535 bytes.
     */
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /**
     * Coordinator side: returns this strategy's setup for the worker.
     * @return Opaque setup bytes, for example a shared-memory region name.
     */
    [[nodiscard]] virtual std::vector<std::byte> offer() = 0;

    /**
     * Worker side: accepts the coordinator's offer.
     * @param offer  The bytes the coordinator's offer() returned.
     * @return The worker's setup bytes, or an error if this worker cannot use
     *         the strategy, in which case negotiation tries the next one.
     */
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, PipelineError> accept(std::span<const std::byte> offer) = 0;

    /**
     * Coordinator side: finishes setup with the worker's answer.
     * @param accepted  The bytes the worker's accept() returned.
     * @return Empty once usable, or an error that fails the connection.
     */
    [[nodiscard]] virtual std::expected<void, PipelineError> complete(std::span<const std::byte> accepted) = 0;

    /**
     * Gives the strategy the connection's payload-frame sender, after the handshake.
     * @param sender  Borrowed; valid until onClosed() returns.
     */
    virtual void attach(IPayloadSender& sender) = 0;

    /**
     * Makes @p payload available to the peer, sending payload frames if needed.
     * @param payload  An inline payload larger than the inline limit.
     * @param scope    The request it belongs to; see endScope().
     * @return The reference for the control frame, or an error that fails the request.
     */
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, PipelineError> send(const Payload& payload, uint64_t scope) = 0;

    /**
     * Turns a reference from the peer's send() into a payload, preferably a
     * view of the received bytes (Payload::view()).
     * @param reference  The reference bytes.
     * @return The payload, or an error that fails the job.
     */
    [[nodiscard]] virtual std::expected<Payload, PipelineError> receive(std::span<const std::byte> reference) = 0;

    /**
     * Receives one of the peer strategy's payload frames.
     * @param frame  The body and its owner.
     */
    virtual void onPayloadFrame(const FrameView& frame) = 0;

    /**
     * Reports that the peer has finished with every payload sent under @p scope;
     * on the coordinator, called when the request's reply arrives.
     * @param scope  The scope passed to send().
     */
    virtual void endScope(uint64_t scope) noexcept { (void)scope; }

    /**
     * Allocates a payload the caller writes in place, in memory this strategy
     * can send without copying.
     * @param bytes  Size to allocate.
     * @return The payload, or nothing to let the caller use ordinary memory.
     */
    [[nodiscard]] virtual std::optional<WritablePayload> allocate(std::size_t bytes)
    {
        (void)bytes;
        return std::nullopt;
    }

    /** The connection closed; release resources held for the peer. */
    virtual void onClosed() noexcept {}
};

/** Creates one connection's strategy instance. */
using PayloadStrategyFactory = std::function<std::unique_ptr<IPayloadStrategy>()>;

/** Settings for streamStrategy(). */
struct StreamOptions
{
    /**
     * Largest payload bytes per frame; the smaller of both sides' values is used.
     * A payload up to this size arrives as one frame whose buffer it then
     * views, with no copy; a larger one is reassembled with one copy.
     */
    uint32_t chunkBytes{16U * 1024U * 1024U};
};

/**
 * Streams payload bytes through the transport in chunked payload frames, for
 * deployments with only a byte stream (stdin and stdout, a socket). Name: "stream".
 * Both sides always support it: negotiation falls back to it.
 * @param options  Settings.
 * @return The factory.
 */
[[nodiscard]] PayloadStrategyFactory streamStrategy(StreamOptions options = {});

/**
 * Moves payload bytes through a consumer's IPayloadStore: send() puts the bytes
 * and sends the handle; receive() gets them. Name: "store".
 * @param store  Borrowed; must outlive every connection using the factory.
 * @return The factory.
 */
[[nodiscard]] PayloadStrategyFactory storeStrategy(IPayloadStore& store);

} // namespace sub0pipeline::remote
