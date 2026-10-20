// include/sub0pipeline/remote/transport.hpp
//
// ITransport -- moves whole frames between a coordinator and a worker -- and
// FrameReader, which splits a byte stream into frames. Link Sub0Pipeline::Remote.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <vector>

namespace sub0pipeline::remote
{

/** Why a transport closed or refused a frame. */
enum class TransportError : uint8_t
{
    kClosed,   ///< Closed by either side.
    kIoError,  ///< The underlying channel failed.
    kProtocol, ///< A peer sent bytes that are not a valid frame.
};

/** Receives a transport's frames and its close notification. */
class IFrameSink
{
public:
    virtual ~IFrameSink() = default;

    /**
     * Called for each complete frame, in arrival order, one call at a time.
     * @param frame  The frame, header included. Valid only during the call.
     */
    virtual void onFrame(std::span<const std::byte> frame) = 0;

    /**
     * Called exactly once, after the last onFrame(), when the transport closes.
     * @param reason  Why it closed.
     */
    virtual void onClosed(TransportError reason) noexcept = 0;
};

/**
 * Moves whole frames to one peer. The library owns framing and encoding;
 * a transport only preserves frame boundaries and order.
 *
 * Contract:
 *   - start() is called once. Afterwards the transport calls the sink from
 *     threads it owns, one call at a time, and calls onClosed() exactly once,
 *     last.
 *   - send() is thread-safe and may be called from sink callbacks.
 *   - close() is idempotent, does not wait, and may be called from any thread,
 *     including sink callbacks.
 *   - Destroying a transport joins its threads; never destroy it from a sink
 *     callback.
 */
class ITransport
{
public:
    virtual ~ITransport() = default;

    /**
     * Starts delivering frames to @p sink.
     * @param sink  Receives frames and the close notification. Borrowed; it
     *              must stay alive until onClosed() has returned.
     */
    virtual void start(IFrameSink& sink) = 0;

    /**
     * Sends one whole frame.
     * @param frame  The frame, header included. Copied before return.
     * @return Empty on success, or why the frame could not be sent.
     */
    [[nodiscard]] virtual std::expected<void, TransportError> send(std::span<const std::byte> frame) = 0;

    /** Closes the connection; the sink then receives onClosed(). */
    virtual void close() noexcept = 0;
};

/**
 * Splits a byte stream into frames, for transports over pipes or sockets.
 * A frame is a u32 little-endian body length, a u8 type, then the body.
 */
class FrameReader
{
public:
    /** Bytes before the body: the u32 length and the u8 type. */
    static constexpr std::size_t kHeaderBytes = 5U;

    /**
     * @param maxBodyBytes  Longest body accepted; a longer one is a protocol error.
     */
    explicit FrameReader(uint32_t maxBodyBytes = 64U * 1024U * 1024U) noexcept : maxBodyBytes_{maxBodyBytes} {}

    /**
     * Appends @p bytes and calls @p onFrame for each frame they complete.
     * @param bytes    Received bytes, in stream order.
     * @param onFrame  Called with each complete frame, header included.
     * @return false if a frame exceeds the body limit; the stream is then
     *         unusable and the transport should close with kProtocol.
     */
    bool feed(std::span<const std::byte> bytes, const std::function<void(std::span<const std::byte>)>& onFrame)
    {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
        std::size_t offset = 0U;
        while (buffer_.size() - offset >= kHeaderBytes)
        {
            uint32_t body = 0U;
            for (std::size_t i = 0U; i < 4U; ++i)
                body |= static_cast<uint32_t>(std::to_integer<uint8_t>(buffer_[offset + i])) << (8U * i);
            if (body > maxBodyBytes_) return false;
            const std::size_t frameBytes = kHeaderBytes + body;
            if (buffer_.size() - offset < frameBytes) break;
            onFrame(std::span<const std::byte>{buffer_.data() + offset, frameBytes});
            offset += frameBytes;
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }

private:
    uint32_t               maxBodyBytes_;
    std::vector<std::byte> buffer_;
};

} // namespace sub0pipeline::remote
