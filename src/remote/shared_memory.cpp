// Placeholder bodies so the shared-memory tests build before the implementation.
#include "sub0pipeline/remote/shared_memory.hpp"

namespace sub0pipeline::remote
{
struct SharedMemoryStrategy::State {};
SharedMemoryStrategy::SharedMemoryStrategy(SharedMemoryOptions) {}
SharedMemoryStrategy::~SharedMemoryStrategy() = default;
std::vector<std::byte> SharedMemoryStrategy::offer() { return {}; }
std::expected<std::vector<std::byte>, PipelineError> SharedMemoryStrategy::accept(std::span<const std::byte>) { return std::unexpected(PipelineError::kUnavailable); }
std::expected<void, PipelineError> SharedMemoryStrategy::complete(std::span<const std::byte>) { return std::unexpected(PipelineError::kUnavailable); }
void SharedMemoryStrategy::attach(IPayloadSender&) {}
std::expected<std::vector<std::byte>, PipelineError> SharedMemoryStrategy::send(const Payload&, uint64_t) { return std::unexpected(PipelineError::kUnavailable); }
std::expected<Payload, PipelineError> SharedMemoryStrategy::receive(std::span<const std::byte>) { return std::unexpected(PipelineError::kUnavailable); }
void SharedMemoryStrategy::onPayloadFrame(const FrameView&) {}
void SharedMemoryStrategy::endScope(uint64_t) noexcept {}
std::optional<WritablePayload> SharedMemoryStrategy::allocate(std::size_t) { return std::nullopt; }
void SharedMemoryStrategy::onClosed() noexcept {}
std::size_t SharedMemoryStrategy::bytesInUse() const { return 1U; }
std::string SharedMemoryStrategy::regionName() const { return {}; }
PayloadStrategyFactory sharedMemoryStrategy(SharedMemoryOptions options)
{
    return [options] { return std::make_unique<SharedMemoryStrategy>(options); };
}
} // namespace sub0pipeline::remote
