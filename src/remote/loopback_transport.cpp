// src/remote/loopback_transport.cpp
//
// In-memory connected transports.

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "sub0pipeline/remote/loopback_transport.hpp"

namespace sub0pipeline::remote
{

using Frame = std::shared_ptr<std::vector<std::byte>>;

struct LoopbackTransport::Shared
{
    std::mutex              mtx;
    std::condition_variable changed;
    bool                    closed{false};
    std::deque<Frame>       inbox[2]; ///< Frames waiting for delivery at each side.
};

std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>> LoopbackTransport::pair()
{
    auto shared = std::make_shared<Shared>();
    return {std::unique_ptr<LoopbackTransport>{new LoopbackTransport{shared, 0U}},
            std::unique_ptr<LoopbackTransport>{new LoopbackTransport{shared, 1U}}};
}

LoopbackTransport::~LoopbackTransport()
{
    close();
    if (delivery_.joinable()) delivery_.join();
}

void LoopbackTransport::start(IFrameSink& sink)
{
    delivery_ = std::jthread{[shared = shared_, side = side_, &sink] {
        auto& inbox = shared->inbox[side];
        std::unique_lock lock{shared->mtx};
        for (;;)
        {
            shared->changed.wait(lock, [&] { return shared->closed || !inbox.empty(); });
            if (inbox.empty()) break; // closed, and everything sent before the close is delivered
            auto frame = std::move(inbox.front());
            inbox.pop_front();
            lock.unlock();
            const std::span<const std::byte> bytes{*frame};
            sink.onFrameBuffer(std::move(frame), bytes);
            lock.lock();
        }
        lock.unlock();
        sink.onClosed(TransportError::kClosed);
    }};
}

std::expected<void, TransportError> LoopbackTransport::send(std::span<const std::byte> frame)
{
    const std::span<const std::byte> parts[] = {frame};
    return sendParts(parts);
}

std::expected<void, TransportError> LoopbackTransport::sendParts(std::span<const std::span<const std::byte>> parts)
{
    // The one copy this medium needs: into the peer's queue.
    std::size_t total = 0U;
    for (auto part : parts) total += part.size();
    auto frame = std::make_shared<std::vector<std::byte>>();
    frame->reserve(total);
    for (auto part : parts) frame->insert(frame->end(), part.begin(), part.end());
    {
        std::lock_guard lock{shared_->mtx};
        if (shared_->closed) return std::unexpected(TransportError::kClosed);
        shared_->inbox[1U - side_].push_back(std::move(frame));
    }
    shared_->changed.notify_all();
    return {};
}

void LoopbackTransport::close() noexcept
{
    {
        std::lock_guard lock{shared_->mtx};
        shared_->closed = true;
    }
    shared_->changed.notify_all();
}

} // namespace sub0pipeline::remote
