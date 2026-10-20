// Placeholder bodies so the remote tests build before the implementation.
#include "sub0pipeline/remote/loopback_transport.hpp"

namespace sub0pipeline::remote
{
struct LoopbackTransport::Shared {};
std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>> LoopbackTransport::pair()
{
    auto shared = std::make_shared<Shared>();
    return {std::unique_ptr<LoopbackTransport>{new LoopbackTransport{shared, 0U}},
            std::unique_ptr<LoopbackTransport>{new LoopbackTransport{shared, 1U}}};
}
LoopbackTransport::~LoopbackTransport() = default;
void LoopbackTransport::start(IFrameSink&) {}
std::expected<void, TransportError> LoopbackTransport::send(std::span<const std::byte>)
{
    return std::unexpected(TransportError::kClosed);
}
void LoopbackTransport::close() noexcept {}
} // namespace sub0pipeline::remote
