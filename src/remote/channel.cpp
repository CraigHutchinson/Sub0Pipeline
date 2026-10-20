// src/remote/channel.cpp
//
// RemoteChannel: handshake and strategy negotiation, request submission with
// back-pressure, replies, cancellation and connection loss on the coordinator side.

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "payload_sender.hpp"
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
std::string kindLabel(const SubGraphRequest& request, uint32_t index)
{
    if (index >= request.jobs().size()) return "job " + std::to_string(index);
    const auto& kind = request.jobs()[index].kind;
    return "job " + std::to_string(index) + " (" + kind.name + "@" + std::to_string(kind.version) + ")";
}
} // namespace

RemoteChannel::RemoteChannel(ITransport& transport, Options options) : transport_{transport}, options_{std::move(options)} {}

RemoteChannel::~RemoteChannel()
{
    transport_.close();
    std::unique_lock lock{mtx_};
    if (started_) changed_.wait(lock, [this] { return closed_; });
}

std::expected<void, ConnectError> RemoteChannel::connect()
{
    Hello hello;
    hello.fingerprint = options_.expectedFingerprint;
    hello.maxInFlight = options_.maxInFlight;
    hello.maxInlineBytes = options_.maxInlineBytes;
    {
        std::lock_guard lock{mtx_};
        if (state_ != State::kIdle) return std::unexpected(ConnectError::kProtocol);
        state_ = State::kHandshaking;
        started_ = true;
        // Streaming is always offered last, so any minor-1 worker can connect.
        auto factories = options_.strategies;
        factories.push_back(streamStrategy());
        for (auto& factory : factories)
        {
            auto candidate = factory ? factory() : nullptr;
            if (!candidate) continue;
            const auto name = candidate->name();
            const bool duplicate = std::any_of(candidates_.begin(), candidates_.end(),
                [&](const auto& c) { return c->name() == name; });
            if (duplicate) continue;
            hello.strategies.push_back({std::string{name}, candidate->offer()});
            candidates_.push_back(std::move(candidate));
        }
    }
    transport_.start(*this);
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

WritablePayload RemoteChannel::allocatePayload(std::size_t size)
{
    IPayloadStrategy* strategy;
    {
        std::lock_guard lock{mtx_};
        strategy = state_ == State::kConnected ? strategy_.get() : nullptr;
    }
    if (strategy)
    {
        if (auto writable = strategy->allocate(size)) return std::move(*writable);
    }
    return makeWritablePayload(size);
}

std::string RemoteChannel::payloadStrategy() const
{
    std::lock_guard lock{mtx_};
    return strategy_ && state_ != State::kHandshaking ? std::string{strategy_->name()} : std::string{};
}

void RemoteChannel::send(std::shared_ptr<const SubGraphRequest> request, Completion done, ReplyHandler onReply)
{
    std::unique_lock lock{mtx_};
    if (state_ != State::kConnected)
    {
        const bool lost = state_ == State::kClosed;
        lock.unlock();
        done.fail(PipelineError::kUnavailable, lost ? "worker connection closed" : "not connected");
        return;
    }
    // Without a negotiated strategy (a minor-0 worker) large inline payloads cannot travel.
    if (!strategy_)
    {
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
    auto* strategy = strategy_.get();
    const auto inlineLimit = inlineLimit_;
    auto entry = std::make_unique<Pending>();
    entry->request = request;
    entry->done = std::move(done);
    entry->onReply = std::move(onReply);
    pending_.emplace(id, std::move(entry));
    lock.unlock();

    // Strategy payloads first: their frames must precede the control frame.
    const auto& jobs = request->jobs();
    std::vector<Payload> references;
    std::vector<const Payload*> overrides;
    if (strategy)
    {
        references.reserve(jobs.size());
        overrides.assign(jobs.size(), nullptr);
        for (std::size_t i = 0U; i < jobs.size(); ++i)
        {
            const auto& input = jobs[i].input;
            if (input.kind() != PayloadKind::kInline || input.bytes().size() <= inlineLimit) continue;
            auto reference = strategy->send(input, id);
            if (!reference)
            {
                failPending(id, reference.error(), kindLabel(*request, static_cast<uint32_t>(i)) + ": payload transfer failed");
                return;
            }
            references.push_back(Payload::reference(*reference));
            overrides[i] = &references.back();
        }
    }
    auto frame = wire::encodeRequest(id, *request, overrides);
    if (!frame)
    {
        failPending(id, PipelineError::kJobFailed, "request does not fit the wire format");
        return;
    }
    if (!transport_.send(*frame))
    {
        failPending(id, PipelineError::kUnavailable, "worker connection lost");
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

void RemoteChannel::failPending(uint64_t id, PipelineError error, const std::string& message)
{
    std::unique_ptr<Pending> failed;
    std::unique_ptr<Queued>  next;
    {
        std::lock_guard lock{mtx_};
        auto found = pending_.find(id);
        if (found == pending_.end()) return; // onClosed already failed it
        failed = std::move(found->second);
        pending_.erase(found);
        next = takeQueued();
    }
    if (strategy_) strategy_->endScope(id);
    failed->done.fail(error, message);
    launch(std::move(next));
}

std::unique_ptr<RemoteChannel::Queued> RemoteChannel::takeQueued()
{
    if (queued_.empty()) return nullptr;
    auto next = std::move(queued_.front());
    queued_.pop_front();
    return next;
}

void RemoteChannel::launch(std::unique_ptr<Queued> next)
{
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
        next = takeQueued();
    }
}

void RemoteChannel::sendCancel(uint64_t id) noexcept
{
    (void)transport_.send(wire::encodeCancel(id));
}

void RemoteChannel::onFrame(std::span<const std::byte> frame)
{
    onFrameBuffer(nullptr, frame);
}

void RemoteChannel::onFrameBuffer(std::shared_ptr<const void> owner, std::span<const std::byte> frame)
{
    const auto type = wire::frameType(frame);
    if (type == FrameType::kReply)
    {
        handleReply(frame);
        return;
    }
    if (type == FrameType::kPayload && strategy_)
    {
        strategy_->onPayloadFrame({frame.subspan(FrameReader::kHeaderBytes), std::move(owner)});
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
            else if (selectStrategy(*hello))
            {
                inFlightLimit_ = std::min(options_.maxInFlight, hello->maxInFlight);
                inlineLimit_ = std::min(options_.maxInlineBytes, hello->maxInlineBytes);
                state_ = State::kConnected;
                changed_.notify_all();
                return;
            }
            else
            {
                rejected_ = ConnectError::kProtocol;
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

bool RemoteChannel::selectStrategy(const Hello& ack)
{
    auto candidates = std::move(candidates_);
    candidates_.clear();
    if (ack.strategies.empty()) return true; // a minor-0 worker: no strategy, Part 2 behaviour
    const auto& chosen = ack.strategies.front();
    for (auto& candidate : candidates)
    {
        if (candidate->name() != chosen.name) continue;
        if (!candidate->complete(chosen.setup)) return false;
        sender_ = std::make_unique<TransportPayloadSender>(transport_);
        candidate->attach(*sender_);
        strategy_ = std::move(candidate);
        return true;
    }
    return false; // the worker chose something not offered
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

    // Strategy outputs are collected before anything else, so the payload
    // state for this reply is released whatever happens to the request.
    for (auto& result : reply.results)
    {
        if (result.output.kind() != PayloadKind::kReference) continue;
        auto payload = strategy_ ? strategy_->receive(result.output.bytes()) : std::unexpected(PipelineError::kJobFailed);
        if (payload)
        {
            result.output = std::move(*payload);
        }
        else
        {
            result.output = {};
            result.status = JobStatus::kFailed;
            result.error = payload.error();
            result.message = "output payload unavailable";
        }
    }

    std::unique_ptr<Pending> entry;
    std::unique_ptr<Queued>  next;
    {
        std::lock_guard lock{mtx_};
        auto found = pending_.find(id);
        if (found == pending_.end()) return;
        entry = std::move(found->second);
        pending_.erase(found);
        next = takeQueued();
    }
    // The worker has finished reading this request's inputs.
    if (strategy_) strategy_->endScope(id);
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
    launch(std::move(next));
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
    if (strategy_) strategy_->onClosed();
    // Last: the destructor may return, and destroy this object, once it sees closed_.
    std::lock_guard lock{mtx_};
    closed_ = true;
    changed_.notify_all();
}

} // namespace sub0pipeline::remote
