// Placeholder bodies so the payload tests build before the implementation.
#include "sub0pipeline/remote/pipe_transport.hpp"

namespace sub0pipeline::remote
{
PipeTransport::PipeTransport(NativeHandle readHandle, NativeHandle writeHandle) noexcept
    : readHandle_{readHandle}, writeHandle_{writeHandle} {}
std::pair<std::unique_ptr<PipeTransport>, std::unique_ptr<PipeTransport>> PipeTransport::pair() { return {}; }
PipeTransport::~PipeTransport() = default;
void PipeTransport::start(IFrameSink&) {}
std::expected<void, TransportError> PipeTransport::send(std::span<const std::byte>) { return std::unexpected(TransportError::kClosed); }
std::expected<void, TransportError> PipeTransport::sendParts(std::span<const std::span<const std::byte>>) { return std::unexpected(TransportError::kClosed); }
void PipeTransport::close() noexcept {}
void PipeTransport::read(IFrameSink&) {}
} // namespace sub0pipeline::remote
