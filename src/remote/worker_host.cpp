// src/remote/worker_host.cpp
//
// WorkerHost: handshake, request admission, running each request as its own
// Pipeline, cancellation and replies on the worker side.

#include <algorithm>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "payload_sender.hpp"
#include "sub0pipeline/executor/scoped_executor.hpp"
#include "sub0pipeline/executor/sequential_executor.hpp"
#include "sub0pipeline/pipeline.hpp"
#include "sub0pipeline/remote/subgraph.hpp"
#include "sub0pipeline/remote/wire.hpp"
#include "sub0pipeline/remote/worker_host.hpp"

namespace sub0pipeline::remote
{

struct WorkerHost::Work
{
    uint64_t             id{0U};
    SubGraphRequest      request;
    std::vector<Payload> inputs; ///< Job inputs with strategy references resolved; views live until the reply is sent.
    std::stop_source     stop;
};

WorkerHost::WorkerHost(const JobRegistry& registry, ITransport& transport, Options options)
    : registry_{registry}, transport_{transport}, options_{options} {}

WorkerHost::~WorkerHost() = default;

void WorkerHost::serve(std::stop_token stop)
{
    transport_.start(*this);
    std::vector<std::jthread> runners;
    runners.reserve(options_.maxConcurrentRequests);
    for (uint32_t i = 0U; i < options_.maxConcurrentRequests; ++i)
        runners.emplace_back([this](std::stop_token runnerStop) { runner(runnerStop); });

    {
        std::unique_lock lock{mtx_};
        changed_.wait(lock, stop, [this] { return closed_; });
    }
    transport_.close();
    std::vector<std::shared_ptr<Work>> running;
    {
        std::unique_lock lock{mtx_};
        changed_.wait(lock, [this] { return closed_; }); // no frame arrives after this
        for (auto& [id, work] : active_) running.push_back(work);
        for (auto& work : queue_) running.push_back(work);
        queue_.clear();
    }
    // Outside the lock: stop requests run the jobs' stop callbacks synchronously.
    for (auto& work : running) work->stop.request_stop();
    for (auto& runner : runners) runner.request_stop();
    changed_.notify_all();
    runners.clear(); // joins
}

void WorkerHost::runner(std::stop_token stop)
{
    for (;;)
    {
        std::shared_ptr<Work> work;
        {
            std::unique_lock lock{mtx_};
            if (!changed_.wait(lock, stop, [this] { return !queue_.empty(); })) return;
            work = std::move(queue_.front());
            queue_.pop_front();
            active_[work->id] = work;
        }
        run(*work);
        std::lock_guard lock{mtx_};
        active_.erase(work->id);
        --admitted_;
    }
}

void WorkerHost::run(Work& work)
{
    const auto& jobs = work.request.jobs();
    const auto count = jobs.size();
    std::vector<Payload> outputs(count);
    std::vector<std::string> errors(count);
    std::vector<PipelineError> codes(count, PipelineError::kJobFailed);
    std::vector<std::vector<const Payload*>> predecessors(count);
    std::vector<std::vector<uint32_t>> predecessorIds(count);
    for (const auto& edge : work.request.edges())
    {
        predecessors[edge.to].push_back(&outputs[edge.from]);
        predecessorIds[edge.to].push_back(edge.from);
    }
    // Jobs run as optional so one failure does not stop independent branches:
    // the set of failures then depends only on the inputs, not on scheduling.
    // A job whose predecessor did not succeed skips itself instead.
    std::vector<uint8_t> skipped(count, 0U);

    Pipeline pipe;
    pipe.reserve(count);
    std::vector<Job> handles;
    handles.reserve(count);
    for (std::size_t i = 0U; i < count; ++i)
    {
        const auto& spec = jobs[i];
        const JobKindFn* fn = registry_.find(spec.kind.name, spec.kind.version);
        handles.push_back(pipe.emplace([&, i, fn](std::stop_token stop) -> std::expected<void, PipelineError> {
            for (auto p : predecessorIds[i])
            {
                if (skipped[p] != 0U || pipe.status(handles[p]) != JobStatus::kDone)
                {
                    skipped[i] = 1U;
                    return {};
                }
            }
            if (!fn)
            {
                errors[i] = "unknown job kind " + jobs[i].kind.name + "@" + std::to_string(jobs[i].kind.version);
                return std::unexpected(PipelineError::kJobFailed);
            }
            JobContext context{work.inputs[i], predecessors[i], outputs[i], errors[i], options_.store, std::move(stop),
                               strategy_.get()};
            auto result = (*fn)(context);
            if (!result) codes[i] = result.error();
            return result;
        }).name(spec.kind.name).optional());
    }
    for (const auto& edge : work.request.edges()) handles[edge.to].succeed(handles[edge.from]);

    std::expected<void, PipelineError> outcome;
    if (options_.executor)
    {
        // Scoped, so this run waits for its own jobs and not for other requests'.
        ScopedExecutor scoped{*options_.executor};
        outcome = pipe.run(scoped, work.stop.get_token());
    }
    else
    {
        SequentialExecutor inline_;
        outcome = pipe.run(inline_, work.stop.get_token());
    }
    pipe.joinOrphans();
    const bool cyclic = !outcome && outcome.error() == PipelineError::kCyclicDependency;

    SubGraphReply reply;
    reply.results.resize(count);
    std::vector<Payload> references;
    references.reserve(count);
    std::vector<const Payload*> overrides(count, nullptr);
    for (std::size_t i = 0U; i < count; ++i)
    {
        auto& result = reply.results[i];
        if (cyclic)
        {
            result.status = JobStatus::kFailed;
            result.error = PipelineError::kCyclicDependency;
            result.message = "the request's edges form a cycle";
            continue;
        }
        result.status = skipped[i] != 0U ? JobStatus::kSkipped : pipe.status(handles[i]);
        result.error = result.status == JobStatus::kCancelled ? PipelineError::kCancelled
                     : result.status == JobStatus::kTimedOut  ? PipelineError::kTimeout
                                                              : codes[i];
        result.message = std::move(errors[i]);
        if (result.status != JobStatus::kDone || !jobs[i].exported) continue;

        result.output = std::move(outputs[i]);
        if (result.output.kind() != PayloadKind::kInline || result.output.bytes().size() <= inlineLimit_) continue;
        if (strategy_)
        {
            auto reference = strategy_->send(result.output, work.id);
            if (reference)
            {
                references.push_back(Payload::reference(*reference));
                overrides[i] = &references.back();
                continue;
            }
            result.output = {};
            result.status = JobStatus::kFailed;
            result.error = reference.error();
            result.message = "output payload transfer failed";
            continue;
        }
        // A minor-0 coordinator: only a store handle can carry a large output.
        auto stored = options_.store ? options_.store->put(result.output.bytes())
                                     : std::unexpected(PipelineError::kJobFailed);
        if (stored && stored->size() <= kMaxHandleBytes)
        {
            result.output = Payload::handle(*stored);
        }
        else
        {
            result.output = {};
            result.status = JobStatus::kFailed;
            result.error = PipelineError::kJobFailed;
            result.message = options_.store ? "storing a large output failed"
                                            : "output exceeds the inline limit and the worker has no store";
        }
    }
    (void)transport_.send(wire::encodeReply(work.id, reply, overrides)); // a closed transport drops it
}

void WorkerHost::onFrame(std::span<const std::byte> frame)
{
    onFrameBuffer(nullptr, frame);
}

void WorkerHost::onFrameBuffer(std::shared_ptr<const void> owner, std::span<const std::byte> frame)
{
    const auto type = wire::frameType(frame);
    if (type == FrameType::kPayload && strategy_)
    {
        strategy_->onPayloadFrame({frame.subspan(FrameReader::kHeaderBytes), std::move(owner)});
        return;
    }
    if (type == FrameType::kHello)
    {
        const auto hello = wire::decodeHello(frame);
        bool already;
        {
            std::lock_guard lock{mtx_};
            already = handshaken_;
        }
        if (!hello || already)
        {
            transport_.close();
            return;
        }
        const auto fingerprint = registry_.fingerprint();
        if (hello->major != kProtocolMajor || hello->fingerprint != fingerprint)
        {
            HelloReject reject;
            reject.reason = hello->major != kProtocolMajor ? RejectReason::kVersion : RejectReason::kFingerprint;
            reject.detail = hello->major != kProtocolMajor
                ? "worker speaks protocol " + std::to_string(kProtocolMajor) + ", coordinator "
                      + std::to_string(hello->major)
                : "worker job registry differs from the one the coordinator expects";
            (void)transport_.send(wire::encodeReject(reject));
            transport_.close();
            return;
        }
        Hello ack;
        ack.fingerprint = fingerprint;
        ack.maxInFlight = options_.maxConcurrentRequests;
        ack.maxInlineBytes = options_.maxInlineBytes;
        // The coordinator's order decides; a strategy this worker cannot accept is skipped.
        auto factories = options_.strategies;
        factories.push_back(streamStrategy());
        for (const auto& offer : hello->strategies)
        {
            for (auto& factory : factories)
            {
                auto candidate = factory ? factory() : nullptr;
                if (!candidate || candidate->name() != offer.name) continue;
                auto accepted = candidate->accept(offer.setup);
                if (!accepted) break;
                sender_ = std::make_unique<TransportPayloadSender>(transport_);
                candidate->attach(*sender_);
                ack.strategies.push_back({offer.name, std::move(*accepted)});
                strategy_ = std::move(candidate);
                break;
            }
            if (strategy_) break;
        }
        {
            std::lock_guard lock{mtx_};
            handshaken_ = true;
            inlineLimit_ = std::min(options_.maxInlineBytes, hello->maxInlineBytes);
        }
        (void)transport_.send(wire::encodeHello(FrameType::kHelloAck, ack));
        return;
    }

    if (type == FrameType::kRequest)
    {
        auto decoded = wire::decodeRequest(frame);
        // Resolve strategy references first, so their payload state is taken
        // even if the request is then refused.
        std::vector<Payload> inputs;
        bool resolved = true;
        if (decoded)
        {
            inputs.reserve(decoded->second.jobs().size());
            for (const auto& job : decoded->second.jobs())
            {
                if (job.input.kind() != PayloadKind::kReference)
                {
                    inputs.push_back(job.input);
                    continue;
                }
                auto payload = strategy_ ? strategy_->receive(job.input.bytes()) : std::unexpected(PipelineError::kJobFailed);
                resolved = resolved && payload.has_value();
                inputs.push_back(payload ? std::move(*payload) : Payload{});
            }
        }
        std::unique_lock lock{mtx_};
        if (!handshaken_ || !decoded)
        {
            lock.unlock();
            transport_.close();
            return;
        }
        if (admitted_ >= options_.maxConcurrentRequests || !resolved)
        {
            lock.unlock();
            SubGraphReply busy;
            busy.results.resize(decoded->second.jobs().size());
            for (auto& result : busy.results)
            {
                result.status = JobStatus::kFailed;
                result.error = resolved ? PipelineError::kBusy : PipelineError::kJobFailed;
                result.message = resolved ? "worker at capacity" : "input payload unavailable";
            }
            (void)transport_.send(wire::encodeReply(decoded->first, busy));
            return;
        }
        ++admitted_;
        auto work = std::make_shared<Work>();
        work->id = decoded->first;
        work->request = std::move(decoded->second);
        work->inputs = std::move(inputs);
        queue_.push_back(std::move(work));
        lock.unlock();
        changed_.notify_all();
        return;
    }

    if (type == FrameType::kCancel)
    {
        const auto id = wire::decodeCancel(frame);
        std::shared_ptr<Work> target;
        {
            std::lock_guard lock{mtx_};
            if (id)
            {
                if (auto found = active_.find(*id); found != active_.end()) target = found->second;
                for (auto& work : queue_)
                {
                    if (work->id == *id) target = work;
                }
            }
        }
        // A queued request still runs, at once and stopped, so its reply frees the coordinator's slot.
        if (target) target->stop.request_stop();
        return;
    }

    transport_.close();
}

void WorkerHost::onClosed(TransportError) noexcept
{
    if (strategy_) strategy_->onClosed();
    std::lock_guard lock{mtx_};
    closed_ = true;
    changed_.notify_all();
}

} // namespace sub0pipeline::remote
