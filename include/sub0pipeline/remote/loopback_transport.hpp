// include/sub0pipeline/remote/loopback_transport.hpp
//
// LoopbackTransport -- an in-memory connected pair of transports, for tests
// and for running a worker inside the coordinator's process. Link
// Sub0Pipeline::Remote.
#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <thread>
#include <utility>

#include "sub0pipeline/remote/transport.hpp"

namespace sub0pipeline::remote
{

/**
 * One end of an in-memory connection. Each end delivers its received frames
 * on its own thread, in order, handing each frame's buffer to the sink
 * (IFrameSink::onFrameBuffer); the copy into the peer's queue is the only
 * one. Closing either end closes both: frames already sent are delivered,
 * then each sink receives onClosed(kClosed).
 */
class LoopbackTransport final : public ITransport
{
public:
    /**
     * Creates two connected ends.
     * @return The ends; frames sent on one arrive at the other.
     */
    [[nodiscard]] static std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>> pair();

    /** Closes the connection and joins this end's delivery thread. */
    ~LoopbackTransport() override;

    LoopbackTransport(const LoopbackTransport&)            = delete;
    LoopbackTransport& operator=(const LoopbackTransport&) = delete;

    void start(IFrameSink& sink) override;
    [[nodiscard]] std::expected<void, TransportError> send(std::span<const std::byte> frame) override;
    [[nodiscard]] std::expected<void, TransportError> sendParts(std::span<const std::span<const std::byte>> parts) override;
    void close() noexcept override;

private:
    struct Shared;

    LoopbackTransport(std::shared_ptr<Shared> shared, std::size_t side) noexcept
        : shared_{std::move(shared)}, side_{side} {}

    std::shared_ptr<Shared> shared_;
    std::size_t             side_;
    std::jthread            delivery_; // last: joined before the state it uses
};

} // namespace sub0pipeline::remote
