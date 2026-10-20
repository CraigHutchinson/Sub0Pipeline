// include/sub0pipeline/remote/wire.hpp
//
// The remote protocol's frames: types, handshake values, and encoding and
// decoding. RemoteChannel and WorkerHost use these; they are public so other
// tools can speak or inspect the protocol. Link Sub0Pipeline::Remote.
//
// All integers are little-endian. Strings are a u16 length and the bytes;
// blobs are a u32 length and the bytes. A frame is a u32 body length, a u8
// type and the body. See docs/design/cross-process-orchestration.md, "Wire format".
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sub0pipeline/remote/subgraph.hpp"

namespace sub0pipeline::remote
{

/** Incompatible protocol changes bump this; peers with different values do not connect. */
inline constexpr uint16_t kProtocolMajor = 1U;
/** Compatible additions bump this; readers skip fields they do not know. */
inline constexpr uint16_t kProtocolMinor = 1U;
/** "S0PL" as a little-endian u32, the first field of a hello. */
inline constexpr uint32_t kProtocolMagic = 0x4C503053U;

/** The frame type byte. */
enum class FrameType : uint8_t
{
    kHello       = 1U, ///< Coordinator to worker: Hello.
    kHelloAck    = 2U, ///< Worker to coordinator: Hello with the worker's values.
    kHelloReject = 3U, ///< Worker to coordinator: HelloReject; the worker then closes.
    kRequest     = 4U, ///< A request id and a SubGraphRequest.
    kReply       = 5U, ///< A request id and a SubGraphReply.
    kCancel      = 6U, ///< A request id to stop.
    kPayload     = 7U, ///< A payload strategy's own frame; the body is strategy-defined.
};

/** A payload strategy as offered or chosen in the handshake. */
struct StrategyOffer
{
    std::string            name;  ///< The strategy's name().
    std::vector<std::byte> setup; ///< Its offer() or accept() bytes.
};

/** Handshake values, sent by both sides. */
struct Hello
{
    uint16_t major{kProtocolMajor};     ///< Protocol major version.
    uint16_t minor{kProtocolMinor};     ///< Protocol minor version.
    uint64_t fingerprint{0U};           ///< Coordinator: the registry it expects. Worker: its registry.
    uint32_t maxInFlight{0U};           ///< Requests this side accepts at once.
    uint32_t maxInlineBytes{0U};        ///< Largest inline payload this side accepts.
    /// Protocol minor 1. Coordinator: its offers, in preference order. Worker:
    /// the chosen strategy, or none. Empty when the peer speaks minor 0.
    std::vector<StrategyOffer> strategies;
};

/** Why a worker refused a connection. Values are part of the wire format. */
enum class RejectReason : uint8_t
{
    kVersion     = 1U, ///< Different protocol major version.
    kFingerprint = 2U, ///< The worker's registry differs from the one expected.
};

/** A worker's refusal. */
struct HelloReject
{
    RejectReason reason{RejectReason::kVersion}; ///< Why.
    std::string  detail;                         ///< Human-readable explanation.
};

namespace wire
{

/**
 * Encodes a hello or hello-ack frame.
 * @param type   FrameType::kHello or FrameType::kHelloAck.
 * @param hello  The values.
 * @return The frame.
 */
[[nodiscard]] std::vector<std::byte> encodeHello(FrameType type, const Hello& hello);

/**
 * Encodes a hello-reject frame.
 * @param reject  The reason and detail.
 * @return The frame.
 */
[[nodiscard]] std::vector<std::byte> encodeReject(const HelloReject& reject);

/**
 * Encodes a request frame.
 * @param id       The request id, echoed by the reply.
 * @param request  The sub-graph.
 * @return The frame, or nothing if the request does not fit the format (a
 *         name over 65,535 bytes, a handle over kMaxHandleBytes, or more than
 *         65,536 jobs).
 */
[[nodiscard]] std::optional<std::vector<std::byte>> encodeRequest(uint64_t id, const SubGraphRequest& request);

/**
 * Encodes a request frame, replacing some job inputs.
 * @param id         The request id.
 * @param request    The sub-graph.
 * @param overrides  One entry per job, or empty; a non-null entry is encoded
 *                   instead of that job's input, typically a strategy reference.
 * @return The frame, or nothing as for encodeRequest(id, request).
 */
[[nodiscard]] std::optional<std::vector<std::byte>> encodeRequest(uint64_t id, const SubGraphRequest& request,
                                                                  std::span<const Payload* const> overrides);

/**
 * Encodes a reply frame.
 * @param id     The request id.
 * @param reply  The results.
 * @return The frame.
 */
[[nodiscard]] std::vector<std::byte> encodeReply(uint64_t id, const SubGraphReply& reply);

/**
 * Encodes a reply frame, replacing some job outputs.
 * @param id         The request id.
 * @param reply      The results.
 * @param overrides  One entry per result, or empty; a non-null entry is
 *                   encoded instead of that result's output.
 * @return The frame.
 */
[[nodiscard]] std::vector<std::byte> encodeReply(uint64_t id, const SubGraphReply& reply,
                                                 std::span<const Payload* const> overrides);

/**
 * Returns the header of a payload frame with a body of @p bodyBytes.
 * @param bodyBytes  The body's size.
 * @return The five header bytes.
 */
[[nodiscard]] std::array<std::byte, 5> payloadFrameHeader(uint32_t bodyBytes) noexcept;

/**
 * Encodes a cancel frame.
 * @param id  The request id to stop.
 * @return The frame.
 */
[[nodiscard]] std::vector<std::byte> encodeCancel(uint64_t id);

/**
 * Reads a frame's type after checking that its length field matches its size.
 * @param frame  A whole frame.
 * @return The type, or nothing for a malformed frame or unknown type.
 */
[[nodiscard]] std::optional<FrameType> frameType(std::span<const std::byte> frame) noexcept;

/**
 * Decodes a hello or hello-ack frame.
 * @param frame  A whole frame.
 * @return The values, or nothing if malformed or the magic differs.
 */
[[nodiscard]] std::optional<Hello> decodeHello(std::span<const std::byte> frame);

/**
 * Decodes a hello-reject frame.
 * @param frame  A whole frame.
 * @return The refusal, or nothing if malformed.
 */
[[nodiscard]] std::optional<HelloReject> decodeReject(std::span<const std::byte> frame);

/**
 * Decodes a request frame, checking edge indices and payload limits.
 * @param frame  A whole frame.
 * @return The id and request, or nothing if malformed.
 */
[[nodiscard]] std::optional<std::pair<uint64_t, SubGraphRequest>> decodeRequest(std::span<const std::byte> frame);

/**
 * Decodes a reply frame.
 * @param frame  A whole frame.
 * @return The id and reply, or nothing if malformed.
 */
[[nodiscard]] std::optional<std::pair<uint64_t, SubGraphReply>> decodeReply(std::span<const std::byte> frame);

/**
 * Decodes a cancel frame.
 * @param frame  A whole frame.
 * @return The request id, or nothing if malformed.
 */
[[nodiscard]] std::optional<uint64_t> decodeCancel(std::span<const std::byte> frame) noexcept;

} // namespace wire
} // namespace sub0pipeline::remote
