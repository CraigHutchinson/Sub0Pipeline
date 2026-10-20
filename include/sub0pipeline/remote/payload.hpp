// include/sub0pipeline/remote/payload.hpp
//
// Payload -- a job's input or output bytes, inline or as an opaque handle --
// and IPayloadStore, which resolves handles. Link Sub0Pipeline::Remote.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include "sub0pipeline/error.hpp"

namespace sub0pipeline::remote
{

/** How a Payload carries its bytes. Values are part of the wire format. */
enum class PayloadKind : uint8_t
{
    kNone   = 0U, ///< No payload.
    kInline = 1U, ///< The bytes themselves, up to the negotiated inline limit.
    kHandle = 2U, ///< An opaque key that both sides resolve through an IPayloadStore.
};

/** Longest handle the wire format carries, in bytes; a content hash fits. */
inline constexpr std::size_t kMaxHandleBytes = 64U;

/**
 * A job's input or output: nothing, inline bytes, or an opaque handle.
 *
 * The library never interprets handle bytes. Inline payloads larger than the
 * limit agreed in the handshake are refused at submission; a worker turns a
 * larger output into a handle through its IPayloadStore.
 */
class Payload
{
public:
    /** Creates an empty payload (PayloadKind::kNone). */
    Payload() = default;

    /**
     * Creates an inline payload holding a copy of @p bytes.
     * @param bytes  The payload bytes.
     * @return The payload.
     */
    [[nodiscard]] static Payload inlineBytes(std::span<const std::byte> bytes)
    {
        return Payload{PayloadKind::kInline, {bytes.begin(), bytes.end()}};
    }

    /**
     * Creates an inline payload that takes ownership of @p bytes.
     * @param bytes  The payload bytes.
     * @return The payload.
     */
    [[nodiscard]] static Payload inlineBytes(std::vector<std::byte> bytes) noexcept
    {
        return Payload{PayloadKind::kInline, std::move(bytes)};
    }

    /**
     * Creates a handle payload holding a copy of @p key.
     * @param key  The opaque handle, at most kMaxHandleBytes long; a longer
     *             one is refused when the payload is sent.
     * @return The payload.
     */
    [[nodiscard]] static Payload handle(std::span<const std::byte> key)
    {
        return Payload{PayloadKind::kHandle, {key.begin(), key.end()}};
    }

    /**
     * Reports how the payload carries its bytes.
     * @return The kind.
     */
    [[nodiscard]] PayloadKind kind() const noexcept { return kind_; }

    /**
     * Returns the inline bytes, or the handle bytes for a handle payload.
     * @return A view valid until the payload is changed or destroyed.
     */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

    /** Compares kind and bytes. */
    [[nodiscard]] bool operator==(const Payload&) const = default;

private:
    Payload(PayloadKind kind, std::vector<std::byte> bytes) noexcept
        : kind_{kind}, bytes_{std::move(bytes)} {}

    PayloadKind            kind_{PayloadKind::kNone};
    std::vector<std::byte> bytes_;
};

/**
 * Resolves payload handles to bytes and stores bytes under new handles.
 *
 * Supplied by the consumer on both sides of a connection, typically over a
 * shared content-addressed cache. Implementations must be thread-safe: the
 * worker calls them from concurrent jobs.
 */
class IPayloadStore
{
public:
    virtual ~IPayloadStore() = default;

    /**
     * Returns the bytes stored under @p handle.
     * @param handle  A handle previously returned by put(), here or by a peer.
     * @return The bytes, or an error such as kJobFailed for an unknown handle.
     */
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, PipelineError>
        get(std::span<const std::byte> handle) = 0;

    /**
     * Stores @p bytes and returns a handle for them.
     * @param bytes  The bytes to store.
     * @return A handle of at most kMaxHandleBytes bytes, or an error.
     */
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, PipelineError>
        put(std::span<const std::byte> bytes) = 0;
};

} // namespace sub0pipeline::remote
