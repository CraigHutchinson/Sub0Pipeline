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

struct LoopbackTransport::Shared
{
    std::mutex                                       mtx;
    std::condition_variable                          changed;
    bool                                             closed{false};
    std::deque<std::vector<std::byte>>               inbox[2]; ///< Frames waiting for delivery at each side.
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
            sink.onFrame(frame);
            lock.lock();
        }
        lock.unlock();
        sink.onClosed(TransportError::kClosed);
    }};
}

std::expected<void, TransportError> LoopbackTransport::send(std::span<const std::byte> frame)
{
    {
        std::lock_guard lock{shared_->mtx};
        if (shared_->closed) return std::unexpected(TransportError::kClosed);
        shared_->inbox[1U - side_].emplace_back(frame.begin(), frame.end());
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
