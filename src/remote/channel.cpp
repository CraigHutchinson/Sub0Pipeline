// src/remote/channel.cpp
//
// RemoteChannel: handshake, request submission with back-pressure, replies,
// cancellation and connection loss on the coordinator side.

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "sub0pipeline/remote/channel.hpp"

namespace sub0pipeline::remote
{

struct RemoteChannel::Pending
{
    struct CancelForward
    {
        RemoteChannel* channel;
        uint64_t       id;
        void operator()() const noexcept { channel->sendCancel(id); }
    };

    std::shared_ptr<const SubGraphRequest>           request;
    Completion                                       done;
    ReplyHandler                                     onReply;
    std::optional<std::stop_callback<CancelForward>> cancel;
};

struct RemoteChannel::Queued
{
    std::shared_ptr<const SubGraphRequest> request;
    Completion                             done;
    ReplyHandler                           onReply;
};

namespace
{
constexpr std::size_t kIdOffset = FrameReader::kHeaderBytes; // a request body starts with its u64 id

void patchId(std::vector<std::byte>& frame, uint64_t id) noexcept
{
    for (std::size_t i = 0U; i < 8U; ++i) frame[kIdOffset + i] = static_cast<std::byte>((id >> (8U * i)) & 0xFFU);
}

std::string kindLabel(const SubGraphRequest& request, uint32_t index)
{
    if (index >= request.jobs().size()) return "job " + std::to_string(index);
    const auto& kind = request.jobs()[index].kind;
    return "job " + std::to_string(index) + " (" + kind.name + "@" + std::to_string(kind.version) + ")";
}
} // namespace

RemoteChannel::RemoteChannel(ITransport& transport, Options options) : transport_{transport}, options_{options} {}

RemoteChannel::~RemoteChannel()
{
    transport_.close();
    std::unique_lock lock{mtx_};
    if (started_) changed_.wait(lock, [this] { return closed_; });
}

std::expected<void, ConnectError> RemoteChannel::connect()
{
    {
        std::lock_guard lock{mtx_};
        if (state_ != State::kIdle) return std::unexpected(ConnectError::kProtocol);
        state_ = State::kHandshaking;
        started_ = true;
    }
    transport_.start(*this);

    Hello hello;
    hello.fingerprint = options_.expectedFingerprint;
    hello.maxInFlight = options_.maxInFlight;
    hello.maxInlineBytes = options_.maxInlineBytes;
    (void)transport_.send(wire::encodeHello(FrameType::kHello, hello)); // a failed send surfaces as onClosed

    std::unique_lock lock{mtx_};
    changed_.wait_for(lock, options_.handshakeTimeout, [this] { return state_ != State::kHandshaking; });
    if (state_ == State::kConnected) return {};
    if (state_ == State::kHandshaking)
    {
        state_ = State::kClosed;
        lock.unlock();
        transport_.close();
        return std::unexpected(ConnectError::kTimeout);
    }
    return std::unexpected(rejected_.value_or(ConnectError::kTransportClosed));
}

Job RemoteChannel::emplace(Pipeline& pipeline, SubGraphRequest request, ReplyHandler onReply)
{
    return pipeline.emplaceDeferred(
        [this, shared = std::make_shared<const SubGraphRequest>(std::move(request)),
         onReply = std::move(onReply)](Completion done) { send(shared, std::move(done), onReply); });
}

void RemoteChannel::submit(const SubGraphRequest& request, Completion done, ReplyHandler onReply)
{
    send(std::make_shared<const SubGraphRequest>(request), std::move(done), std::move(onReply));
}

uint32_t RemoteChannel::freeCapacity() const
{
    std::lock_guard lock{mtx_};
    if (state_ != State::kConnected) return 0U;
    const auto inFlight = static_cast<uint32_t>(pending_.size());
    const auto queued = static_cast<uint32_t>(queued_.size());
    return (inFlightLimit_ > inFlight ? inFlightLimit_ - inFlight : 0U)
         + (options_.queueCapacity > queued ? options_.queueCapacity - queued : 0U);
}

Hello RemoteChannel::peer() const
{
    std::lock_guard lock{mtx_};
    return peer_;
}

void RemoteChannel::send(std::shared_ptr<const SubGraphRequest> request, Completion done, ReplyHandler onReply)
{
    // Encode before taking a slot, so a request that cannot be sent never holds one.
    auto frame = wire::encodeRequest(0U, *request);
    if (!frame)
    {
        done.fail(PipelineError::kJobFailed, "request does not fit the wire format");
        return;
    }

    std::unique_lock lock{mtx_};
    if (state_ != State::kConnected)
    {
        const bool lost = state_ == State::kClosed;
        lock.unlock();
        done.fail(PipelineError::kUnavailable, lost ? "worker connection closed" : "not connected");
        return;
    }
    for (uint32_t i = 0U; i < request->jobs().size(); ++i)
    {
        const auto& input = request->jobs()[i].input;
        if (input.kind() == PayloadKind::kInline && input.bytes().size() > inlineLimit_)
        {
            lock.unlock();
            done.fail(PipelineError::kJobFailed, kindLabel(*request, i) + ": inline input exceeds the inline limit");
            return;
        }
    }
    if (pending_.size() >= inFlightLimit_)
    {
        if (queued_.size() < options_.queueCapacity)
        {
            queued_.push_back(std::make_unique<Queued>(Queued{std::move(request), std::move(done), std::move(onReply)}));
            return;
        }
        lock.unlock();
        done.fail(PipelineError::kBusy, "remote channel at capacity");
        return;
    }
    const auto id = nextId_++;
    auto entry = std::make_unique<Pending>();
    entry->request = std::move(request);
    entry->done = std::move(done);
    entry->onReply = std::move(onReply);
    pending_.emplace(id, std::move(entry));
    lock.unlock();

    patchId(*frame, id);
    if (!transport_.send(*frame))
    {
        // onClosed may already have failed it; otherwise fail it here.
        std::unique_ptr<Pending> failed;
        {
            std::lock_guard relock{mtx_};
            if (auto found = pending_.find(id); found != pending_.end())
            {
                failed = std::move(found->second);
                pending_.erase(found);
            }
        }
        if (failed) failed->done.fail(PipelineError::kUnavailable, "worker connection lost");
        return;
    }

    // Registered after the send so that a cancel never overtakes its request.
    lock.lock();
    if (auto found = pending_.find(id); found != pending_.end())
    {
        auto& pending = *found->second;
        pending.cancel.emplace(pending.done.stopToken(), Pending::CancelForward{this, id});
    }
}

void RemoteChannel::sendCancel(uint64_t id) noexcept
{
    (void)transport_.send(wire::encodeCancel(id));
}

void RemoteChannel::onFrame(std::span<const std::byte> frame)
{
    const auto type = wire::frameType(frame);
    if (type == FrameType::kReply)
    {
        handleReply(frame);
        return;
    }

    std::unique_lock lock{mtx_};
    if (state_ == State::kHandshaking && type == FrameType::kHelloAck)
    {
        if (auto hello = wire::decodeHello(frame))
        {
            peer_ = *hello;
            if (hello->major != kProtocolMajor)
            {
                rejected_ = ConnectError::kVersionMismatch;
            }
            else if (hello->fingerprint != options_.expectedFingerprint)
            {
                rejected_ = ConnectError::kFingerprintMismatch;
            }
            else
            {
                inFlightLimit_ = std::min(options_.maxInFlight, hello->maxInFlight);
                inlineLimit_ = std::min(options_.maxInlineBytes, hello->maxInlineBytes);
                state_ = State::kConnected;
                changed_.notify_all();
                return;
            }
        }
    }
    else if (state_ == State::kHandshaking && type == FrameType::kHelloReject)
    {
        const auto reject = wire::decodeReject(frame);
        rejected_ = reject && reject->reason == RejectReason::kFingerprint ? ConnectError::kFingerprintMismatch
                  : reject                                                 ? ConnectError::kVersionMismatch
                                                                           : ConnectError::kProtocol;
    }
    else if (state_ == State::kHandshaking)
    {
        rejected_ = ConnectError::kProtocol;
    }
    // Anything else is a protocol error, and every path here ends the connection.
    if (state_ == State::kHandshaking) state_ = State::kClosed;
    changed_.notify_all();
    lock.unlock();
    transport_.close();
}

void RemoteChannel::handleReply(std::span<const std::byte> frame)
{
    auto decoded = wire::decodeReply(frame);
    if (!decoded)
    {
        transport_.close();
        return;
    }
    auto& [id, reply] = *decoded;

    std::unique_ptr<Pending> entry;
    std::unique_ptr<Queued>  next;
    {
        std::lock_guard lock{mtx_};
        auto found = pending_.find(id);
        if (found == pending_.end()) return;
        entry = std::move(found->second);
        pending_.erase(found);
        if (!queued_.empty())
        {
            next = std::move(queued_.front());
            queued_.pop_front();
        }
    }
    // Waits for a cancel being sent on another thread; no lock is held.
    entry->cancel.reset();

    if (entry->done.stopToken().stop_requested())
    {
        entry->done.succeed(); // the job already finished as cancelled; this only releases the orphan
    }
    else
    {
        if (entry->onReply) entry->onReply(reply);
        if (const auto failure = reply.firstFailure())
        {
            const auto& result = reply.results[*failure];
            const auto error = result.status == JobStatus::kCancelled ? PipelineError::kCancelled
                             : result.status == JobStatus::kTimedOut  ? PipelineError::kTimeout
                                                                      : result.error;
            auto message = kindLabel(*entry->request, *failure) + " failed";
            if (!result.message.empty()) message += ": " + result.message;
            entry->done.fail(error, message);
        }
        else if (reply.results.size() != entry->request->jobs().size())
        {
            entry->done.fail(PipelineError::kJobFailed, "reply does not match the request");
        }
        else
        {
            entry->done.succeed();
        }
    }

    // A queued request whose job was cancelled while it waited is not sent.
    while (next)
    {
        if (!next->done.stopToken().stop_requested())
        {
            send(std::move(next->request), std::move(next->done), std::move(next->onReply));
            return;
        }
        next->done.succeed();
        std::lock_guard lock{mtx_};
        next.reset();
        if (!queued_.empty())
        {
            next = std::move(queued_.front());
            queued_.pop_front();
        }
    }
}

void RemoteChannel::onClosed(TransportError) noexcept
{
    std::unordered_map<uint64_t, std::unique_ptr<Pending>> pending;
    std::deque<std::unique_ptr<Queued>> queued;
    {
        std::lock_guard lock{mtx_};
        if (state_ == State::kHandshaking && !rejected_) rejected_ = ConnectError::kTransportClosed;
        state_ = State::kClosed;
        pending.swap(pending_);
        queued.swap(queued_);
    }
    for (auto& [id, entry] : pending)
    {
        entry->cancel.reset();
        entry->done.fail(PipelineError::kUnavailable, "worker connection lost");
    }
    for (auto& entry : queued) entry->done.fail(PipelineError::kUnavailable, "worker connection lost");
    // Last: the destructor may return, and destroy this object, once it sees closed_.
    std::lock_guard lock{mtx_};
    closed_ = true;
    changed_.notify_all();
}

} // namespace sub0pipeline::remote
