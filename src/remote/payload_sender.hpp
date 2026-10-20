// src/remote/payload_sender.hpp
//
// The library's IPayloadSender: adds the payload frame header and sends the
// frame's parts on the connection's transport without concatenating them.
#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "sub0pipeline/remote/strategy.hpp"
#include "sub0pipeline/remote/wire.hpp"

namespace sub0pipeline::remote
{

class TransportPayloadSender final : public IPayloadSender
{
public:
    explicit TransportPayloadSender(ITransport& transport) noexcept : transport_{transport} {}

    std::expected<void, TransportError> sendPayloadFrame(std::span<const std::span<const std::byte>> parts) override
    {
        constexpr std::size_t kMaxParts = 8U;
        if (parts.size() + 1U > kMaxParts) return std::unexpected(TransportError::kProtocol);
        std::size_t body = 0U;
        for (auto part : parts) body += part.size();
        if (body > UINT32_MAX) return std::unexpected(TransportError::kProtocol);
        const auto header = wire::payloadFrameHeader(static_cast<uint32_t>(body));
        std::array<std::span<const std::byte>, kMaxParts> all{};
        all[0] = header;
        for (std::size_t i = 0U; i < parts.size(); ++i) all[i + 1U] = parts[i];
        return transport_.sendParts(std::span<const std::span<const std::byte>>{all.data(), parts.size() + 1U});
    }

private:
    ITransport& transport_;
};

} // namespace sub0pipeline::remote
