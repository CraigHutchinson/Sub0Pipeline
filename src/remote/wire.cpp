// Placeholder bodies so the remote tests build before the implementation.
#include "sub0pipeline/remote/wire.hpp"

namespace sub0pipeline::remote::wire
{
std::vector<std::byte> encodeHello(FrameType, const Hello&) { return {}; }
std::vector<std::byte> encodeReject(const HelloReject&) { return {}; }
std::optional<std::vector<std::byte>> encodeRequest(uint64_t, const SubGraphRequest&) { return std::nullopt; }
std::vector<std::byte> encodeReply(uint64_t, const SubGraphReply&) { return {}; }
std::vector<std::byte> encodeCancel(uint64_t) { return {}; }
std::optional<FrameType> frameType(std::span<const std::byte>) noexcept { return std::nullopt; }
std::optional<Hello> decodeHello(std::span<const std::byte>) { return std::nullopt; }
std::optional<HelloReject> decodeReject(std::span<const std::byte>) { return std::nullopt; }
std::optional<std::pair<uint64_t, SubGraphRequest>> decodeRequest(std::span<const std::byte>) { return std::nullopt; }
std::optional<std::pair<uint64_t, SubGraphReply>> decodeReply(std::span<const std::byte>) { return std::nullopt; }
std::optional<uint64_t> decodeCancel(std::span<const std::byte>) noexcept { return std::nullopt; }
} // namespace sub0pipeline::remote::wire
