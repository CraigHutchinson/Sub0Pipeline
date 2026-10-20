// Placeholder bodies so the remote tests build before the implementation.
#include "sub0pipeline/remote/channel.hpp"

namespace sub0pipeline::remote
{
struct RemoteChannel::Pending {};
struct RemoteChannel::Queued {};
RemoteChannel::RemoteChannel(ITransport& transport, Options options) : transport_{transport}, options_{options} {}
RemoteChannel::~RemoteChannel() = default;
std::expected<void, ConnectError> RemoteChannel::connect() { return std::unexpected(ConnectError::kTransportClosed); }
Job RemoteChannel::emplace(Pipeline& pipeline, SubGraphRequest, ReplyHandler)
{
    return pipeline.emplaceDeferred([](Completion done) { done.fail(PipelineError::kUnavailable); });
}
void RemoteChannel::submit(const SubGraphRequest&, Completion done, ReplyHandler) { done.fail(PipelineError::kUnavailable); }
uint32_t RemoteChannel::freeCapacity() const { return 0U; }
Hello RemoteChannel::peer() const { return {}; }
void RemoteChannel::onFrame(std::span<const std::byte>) {}
void RemoteChannel::onClosed(TransportError) noexcept {}
void RemoteChannel::send(std::shared_ptr<const SubGraphRequest>, Completion, ReplyHandler) {}
void RemoteChannel::sendCancel(uint64_t) noexcept {}
void RemoteChannel::handleReply(std::span<const std::byte>) {}
} // namespace sub0pipeline::remote
