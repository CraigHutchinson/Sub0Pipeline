// include/sub0pipeline/remote/payload.hpp
//
// Payload -- a job's input or output bytes, inline or as an opaque handle --
// and IPayloadStore, which resolves handles. Link Sub0Pipeline::Remote.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "sub0pipeline/error.hpp"

namespace sub0pipeline::remote
{

/** How a Payload carries its bytes. Values are part of the wire format. */
enum class PayloadKind : uint8_t
{
    kNone      = 0U, ///< No payload.
    kInline    = 1U, ///< The bytes themselves, owned or viewed (see Payload::view()).
    kHandle    = 2U, ///< An opaque key that both sides resolve through an IPayloadStore.
    kReference = 3U, ///< A payload strategy's reference; seen only between the wire layer and the strategy.
};

/** Longest handle the wire format carries, in bytes; a content hash fits. */
inline constexpr std::size_t kMaxHandleBytes = 64U;

/**
 * A job's input or output: nothing, inline bytes, or an opaque handle.
 *
 * Inline bytes are either owned by the payload or a view of storage kept alive
 * by a shared owner (Payload::view()); bytes() returns a span in both cases,
 * and copying a view shares the owner rather than the bytes. Payload
 * strategies hand received bytes over as views, so the library adds no copy.
 * The library never interprets handle bytes.
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
     * Creates an inline payload that views @p bytes without copying them.
     * @param bytes  The bytes; they must stay valid and unchanged while @p owner lives.
     * @param owner  Keeps the bytes alive; shared by every copy of the payload.
     * @return The payload.
     */
    [[nodiscard]] static Payload view(std::span<const std::byte> bytes, std::shared_ptr<const void> owner) noexcept
    {
        Payload payload;
        payload.kind_ = PayloadKind::kInline;
        payload.view_ = bytes;
        payload.owner_ = std::move(owner);
        return payload;
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
     * Creates a strategy reference. Used by the wire layer and strategies.
     * @param reference  The strategy's reference bytes.
     * @return The payload.
     */
    [[nodiscard]] static Payload reference(std::span<const std::byte> reference)
    {
        return Payload{PayloadKind::kReference, {reference.begin(), reference.end()}};
    }

    /**
     * Reports how the payload carries its bytes.
     * @return The kind.
     */
    [[nodiscard]] PayloadKind kind() const noexcept { return kind_; }

    /**
     * Returns the inline bytes, or the handle or reference bytes.
     * @return A view valid while this payload, or a copy sharing its owner, lives.
     */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        return owner_ ? view_ : std::span<const std::byte>{bytes_};
    }

    /**
     * Returns the shared owner of a view payload.
     * @return The owner, or nullptr for a payload that owns its bytes.
     */
    [[nodiscard]] const std::shared_ptr<const void>& owner() const noexcept { return owner_; }

    /** Compares kind and byte content. */
    [[nodiscard]] bool operator==(const Payload& other) const noexcept
    {
        const auto a = bytes();
        const auto b = other.bytes();
        return kind_ == other.kind_ && std::equal(a.begin(), a.end(), b.begin(), b.end());
    }

private:
    Payload(PayloadKind kind, std::vector<std::byte> bytes) noexcept
        : kind_{kind}, bytes_{std::move(bytes)} {}

    PayloadKind                 kind_{PayloadKind::kNone};
    std::vector<std::byte>      bytes_;
    std::span<const std::byte>  view_;
    std::shared_ptr<const void> owner_; // set for views
};

/** A payload whose bytes the caller may write before sending it. */
struct WritablePayload
{
    Payload              payload; ///< Send this; it views the writable bytes.
    std::span<std::byte> bytes;   ///< Write here; valid while @ref payload or a copy lives.
};

/**
 * Allocates a writable payload in ordinary memory, without zeroing it.
 * @param size  Bytes to allocate.
 * @return The payload and its writable bytes.
 */
[[nodiscard]] inline WritablePayload makeWritablePayload(std::size_t size)
{
    std::shared_ptr<std::byte[]> storage{new std::byte[size == 0U ? 1U : size]};
    std::span<std::byte> bytes{storage.get(), size};
    return {Payload::view(bytes, std::shared_ptr<const void>{storage, storage.get()}), bytes};
}

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
