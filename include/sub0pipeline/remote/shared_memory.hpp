// include/sub0pipeline/remote/shared_memory.hpp
//
// SharedMemoryStrategy -- the "shm" payload strategy for workers on the same
// machine. Link Sub0Pipeline::Remote.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sub0pipeline/remote/strategy.hpp"

namespace sub0pipeline::remote
{

/** Settings for sharedMemoryStrategy(). */
struct SharedMemoryOptions
{
    /** Size of the region this side writes payloads into. Each side maps its own and the peer's. */
    std::size_t   regionBytes{64U * 1024U * 1024U};
    /** Used for a payload that does not fit the region (too large, or the region is full). */
    StreamOptions fallback{};
};

/**
 * Moves payload bytes through shared memory, for workers on the same machine.
 * Name: "shm".
 *
 * Each side creates one region (a Windows named file mapping, a POSIX
 * shm_open segment) for the payloads it sends and maps the peer's. Only the
 * owner allocates in a region, from bookkeeping in its private memory, so a
 * crashed peer cannot corrupt it and no cross-process lock exists. A payload
 * is copied into the region once on send, or not at all if it was allocated
 * there (allocate(), JobContext::allocateOutput(), RemoteChannel::allocatePayload());
 * the receiver gets a view of the region.
 *
 * Lifetimes: a coordinator's input stays allocated until the request's reply
 * arrives (or the connection closes); the worker must not keep its view past
 * sending the reply. A worker's output stays allocated until the coordinator
 * drops the last copy of the reply's Payload, which sends a release; that view
 * stays readable even after the worker has gone. On POSIX the coordinator
 * removes both names once the handshake has mapped them, so the segments then
 * live only while mapped; Windows frees a mapping with its last handle.
 *
 * A worker that cannot open the coordinator's region -- another machine, or a
 * container without the shared namespace -- refuses it in the handshake, and
 * negotiation moves on to the next strategy.
 */
class SharedMemoryStrategy final : public IPayloadStrategy
{
public:
    /**
     * @param options  Settings.
     */
    explicit SharedMemoryStrategy(SharedMemoryOptions options = {});
    ~SharedMemoryStrategy() override;

    SharedMemoryStrategy(const SharedMemoryStrategy&)            = delete;
    SharedMemoryStrategy& operator=(const SharedMemoryStrategy&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override { return "shm"; }
    [[nodiscard]] std::vector<std::byte> offer() override;
    [[nodiscard]] std::expected<std::vector<std::byte>, PipelineError> accept(std::span<const std::byte> offer) override;
    [[nodiscard]] std::expected<void, PipelineError> complete(std::span<const std::byte> accepted) override;
    void attach(IPayloadSender& sender) override;
    [[nodiscard]] std::expected<std::vector<std::byte>, PipelineError> send(const Payload& payload, uint64_t scope) override;
    [[nodiscard]] std::expected<Payload, PipelineError> receive(std::span<const std::byte> reference) override;
    void onPayloadFrame(const FrameView& frame) override;
    void endScope(uint64_t scope) noexcept override;
    [[nodiscard]] std::optional<WritablePayload> allocate(std::size_t bytes) override;
    void onClosed() noexcept override;

    /**
     * Reports bytes allocated in this side's region, including alignment.
     * @return The bytes in use; 0 once every payload has been released.
     */
    [[nodiscard]] std::size_t bytesInUse() const;

    /**
     * Returns the name of this side's region.
     * @return The name, or empty before the handshake.
     */
    [[nodiscard]] std::string regionName() const;

private:
    struct State;
    std::shared_ptr<State>            state_;    ///< Shared with payload leases, which may outlive the strategy.
    std::unique_ptr<IPayloadStrategy> fallback_; ///< Streams what does not fit the region.
};

/**
 * Returns a factory for SharedMemoryStrategy.
 * @param options  Settings.
 * @return The factory.
 */
[[nodiscard]] PayloadStrategyFactory sharedMemoryStrategy(SharedMemoryOptions options = {});

} // namespace sub0pipeline::remote
