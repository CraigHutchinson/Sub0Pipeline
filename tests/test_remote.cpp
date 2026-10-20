// tests/test_remote.cpp
//
// Remote sub-graphs over the loopback transport: registry, wire format,
// handshake, end-to-end runs, errors, worker loss, back-pressure and cancel.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <doctest.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sub0pipeline/remote/channel.hpp"
#include "sub0pipeline/remote/loopback_transport.hpp"
#include "sub0pipeline/remote/worker_host.hpp"
#include "sub0pipeline/sub0pipeline.hpp"

using namespace sub0pipeline;
using namespace sub0pipeline::remote;
using namespace std::chrono_literals;

namespace
{

bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds limit = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

std::vector<std::byte> bytes(std::string_view text)
{
    std::vector<std::byte> out(text.size());
    std::transform(text.begin(), text.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

std::string text(std::span<const std::byte> data)
{
    std::string out(data.size(), '\0');
    std::transform(data.begin(), data.end(), out.begin(), [](std::byte b) { return static_cast<char>(b); });
    return out;
}

class MemoryStore final : public IPayloadStore
{
public:
    std::expected<std::vector<std::byte>, PipelineError> get(std::span<const std::byte> handle) override
    {
        std::lock_guard lock{mtx_};
        auto found = items_.find(text(handle));
        if (found == items_.end()) return std::unexpected(PipelineError::kJobFailed);
        return found->second;
    }
    std::expected<std::vector<std::byte>, PipelineError> put(std::span<const std::byte> data) override
    {
        std::lock_guard lock{mtx_};
        auto key = "item-" + std::to_string(items_.size());
        items_[key] = {data.begin(), data.end()};
        return bytes(key);
    }
private:
    std::mutex mtx_;
    std::map<std::string, std::vector<std::byte>> items_;
};

// Kinds shared by the tests; "block" waits for release() or a stop request.
struct Kinds
{
    std::atomic<int>  blocked{0};
    std::atomic<int>  stopsSeen{0};
    std::atomic<bool> released{false};

    void registerAll(JobRegistry& registry)
    {
        registry.add({"upper", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
            auto s = text(ctx.input().bytes());
            for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            ctx.setOutput(Payload::inlineBytes(bytes(s)));
            return {};
        });
        registry.add({"concat", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
            std::string s;
            for (std::size_t i = 0; i < ctx.predecessorCount(); ++i) s += text(ctx.predecessor(i).bytes());
            s += text(ctx.input().bytes());
            ctx.setOutput(Payload::inlineBytes(bytes(s)));
            return {};
        });
        registry.add({"fail", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
            ctx.setError("bad input " + text(ctx.input().bytes()));
            return std::unexpected(PipelineError::kJobFailed);
        });
        registry.add({"block", 1}, [this](JobContext& ctx) -> std::expected<void, PipelineError> {
            ++blocked;
            while (!released.load() && !ctx.stopToken().stop_requested()) std::this_thread::sleep_for(1ms);
            if (ctx.stopToken().stop_requested())
            {
                ++stopsSeen;
                return std::unexpected(PipelineError::kCancelled);
            }
            return {};
        });
        registry.add({"big", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
            ctx.setOutput(Payload::inlineBytes(std::vector<std::byte>(4096U, std::byte{0x5A})));
            return {};
        });
    }
};

// One coordinator channel connected over loopback to one worker host.
struct Rig
{
    std::unique_ptr<LoopbackTransport> coordinatorEnd;
    std::unique_ptr<LoopbackTransport> workerEnd;
    PriorityExecutor                   workerPool{{.threadCount = 4}};
    JobRegistry                        registry;
    Kinds                              kinds;
    MemoryStore                        store;
    std::optional<WorkerHost>          host;
    std::jthread                       serving;
    std::optional<RemoteChannel>       channel;

    Rig()
    {
        auto [a, b] = LoopbackTransport::pair();
        coordinatorEnd = std::move(a);
        workerEnd = std::move(b);
        kinds.registerAll(registry);
    }

    void startWorker(WorkerHost::Options options = {})
    {
        if (!options.executor) options.executor = &workerPool;
        if (!options.store) options.store = &store;
        host.emplace(registry, *workerEnd, options);
        serving = std::jthread{[this](std::stop_token stop) { host->serve(stop); }};
    }

    std::expected<void, ConnectError> connect(RemoteChannel::Options options = {})
    {
        if (options.expectedFingerprint == 0U) options.expectedFingerprint = registry.fingerprint();
        channel.emplace(*coordinatorEnd, options);
        return channel->connect();
    }

    ~Rig()
    {
        kinds.released = true;
        channel.reset();
        if (serving.joinable())
        {
            serving.request_stop();
            serving.join();
        }
        host.reset();
    }
};

SubGraphRequest single(std::string_view kind, std::string_view input)
{
    SubGraphRequest request;
    request.add({std::string{kind}, 1}, Payload::inlineBytes(bytes(input)), true);
    return request;
}

} // namespace

TEST_CASE("Remote registry: duplicates refused, lookup by name and version, order-free fingerprint")
{
    JobRegistry registry;
    auto noop = [](JobContext&) -> std::expected<void, PipelineError> { return {}; };
    CHECK(registry.add({"a", 1}, noop));
    CHECK(registry.add({"a", 2}, noop));
    CHECK_FALSE(registry.add({"a", 1}, noop));
    CHECK_FALSE(registry.add({"b", 1}, {}));
    CHECK(registry.find("a", 2) != nullptr);
    CHECK(registry.find("a", 3) == nullptr);
    CHECK(registry.kinds().size() == 2U);

    const std::vector<JobKindId> forward{{"a", 1}, {"a", 2}};
    const std::vector<JobKindId> reverse{{"a", 2}, {"a", 1}};
    const std::vector<JobKindId> bumped{{"a", 1}, {"a", 3}};
    CHECK(registryFingerprint(forward) == registryFingerprint(reverse));
    CHECK(registryFingerprint(forward) == registry.fingerprint());
    CHECK(registryFingerprint(forward) != registryFingerprint(bumped));
}

TEST_CASE("Remote wire: request and reply round-trip; malformed frames are refused")
{
    SubGraphRequest request;
    auto a = request.add({"upper", 1}, Payload::inlineBytes(bytes("x")));
    auto b = request.add({"concat", 7}, Payload::handle(bytes("key")), true);
    CHECK(request.edge(a, b));
    CHECK_FALSE(request.edge(a, a));
    CHECK_FALSE(request.edge(a, 9U));

    auto frame = wire::encodeRequest(42U, request);
    REQUIRE(frame.has_value());
    CHECK(wire::frameType(*frame) == FrameType::kRequest);
    auto decoded = wire::decodeRequest(*frame);
    REQUIRE(decoded.has_value());
    CHECK(decoded->first == 42U);
    REQUIRE(decoded->second.jobs().size() == 2U);
    CHECK(decoded->second.jobs()[1].kind == JobKindId{"concat", 7});
    CHECK(decoded->second.jobs()[1].input == Payload::handle(bytes("key")));
    CHECK(decoded->second.jobs()[1].exported);
    REQUIRE(decoded->second.edges().size() == 1U);
    CHECK(decoded->second.edges()[0].to == b);

    SubGraphReply reply;
    reply.results.push_back({JobStatus::kDone, PipelineError::kJobFailed, "", Payload::inlineBytes(bytes("out"))});
    reply.results.push_back({JobStatus::kFailed, PipelineError::kTimeout, "slow", {}});
    auto replyFrame = wire::encodeReply(42U, reply);
    auto decodedReply = wire::decodeReply(replyFrame);
    REQUIRE(decodedReply.has_value());
    CHECK(decodedReply->second.results[0].output == reply.results[0].output);
    CHECK(decodedReply->second.results[1].error == PipelineError::kTimeout);
    CHECK(decodedReply->second.results[1].message == "slow");
    CHECK(decodedReply->second.firstFailure() == 1U);

    auto truncated = *frame;
    truncated.pop_back();
    CHECK_FALSE(wire::frameType(truncated).has_value());
    CHECK_FALSE(wire::decodeRequest(truncated).has_value());
    CHECK_FALSE(wire::decodeHello(replyFrame).has_value());

    SubGraphRequest oversized;
    oversized.add({"k", 1}, Payload::handle(std::vector<std::byte>(kMaxHandleBytes + 1U)));
    CHECK_FALSE(wire::encodeRequest(1U, oversized).has_value());
}

TEST_CASE("Remote FrameReader: frames survive arbitrary splits; oversized bodies are refused")
{
    auto one = wire::encodeCancel(7U);
    auto two = wire::encodeCancel(8U);
    std::vector<std::byte> stream{one.begin(), one.end()};
    stream.insert(stream.end(), two.begin(), two.end());
    FrameReader reader;
    std::vector<uint64_t> ids;
    for (auto b : stream)
    {
        CHECK(reader.feed(std::span<const std::byte>{&b, 1U}, [&](std::span<const std::byte> frame) {
            ids.push_back(wire::decodeCancel(frame).value_or(0U));
        }));
    }
    CHECK(ids == std::vector<uint64_t>{7U, 8U});

    FrameReader small{4U};
    CHECK_FALSE(small.feed(one, [](std::span<const std::byte>) {}));
}

TEST_CASE("Remote: a one-job request completes the remote job with its exported output")
{
    Rig rig;
    rig.startWorker();
    REQUIRE(rig.connect().has_value());
    CHECK(rig.channel->peer().fingerprint == rig.registry.fingerprint());

    Pipeline pipe;
    std::string output;
    auto remote = rig.channel->emplace(pipe, single("upper", "hello"), [&](const SubGraphReply& reply) {
        output = text(reply.results.at(0).output.bytes());
    }).name("remote-upper");
    std::string seenByDependant;
    (void)pipe.emplace([&] { seenByDependant = output; }).succeed(remote);
    REQUIRE(pipe.runInline().has_value());
    CHECK(output == "HELLO");
    CHECK(seenByDependant == "HELLO");
    CHECK(pipe.status(remote) == JobStatus::kDone);
}

TEST_CASE("Remote: a multi-job sub-graph passes outputs along edges and returns only exported ones")
{
    Rig rig;
    rig.startWorker();
    REQUIRE(rig.connect().has_value());

    SubGraphRequest request;
    auto a = request.add({"upper", 1}, Payload::inlineBytes(bytes("ab")));
    auto b = request.add({"upper", 1}, Payload::inlineBytes(bytes("cd")));
    auto c = request.add({"concat", 1}, Payload::inlineBytes(bytes("!")), true);
    request.edge(a, c);
    request.edge(b, c);

    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    SubGraphReply got;
    auto remote = rig.channel->emplace(pipe, request, [&](const SubGraphReply& reply) { got = reply; });
    REQUIRE(pipe.run(pool).has_value());
    CHECK(pipe.status(remote) == JobStatus::kDone);
    REQUIRE(got.results.size() == 3U);
    CHECK(got.results[a].status == JobStatus::kDone);
    CHECK(got.results[a].output.kind() == PayloadKind::kNone);
    CHECK(text(got.results[c].output.bytes()) == "ABCD!");

    // req-determinism: the same kinds run in process give the same bytes.
    Payload outA, outB, outC;
    std::string err;
    const auto* upper = rig.registry.find("upper", 1);
    const auto* concat = rig.registry.find("concat", 1);
    const auto inA = Payload::inlineBytes(bytes("ab"));
    const auto inB = Payload::inlineBytes(bytes("cd"));
    const auto inC = Payload::inlineBytes(bytes("!"));
    JobContext ctxA{inA, {}, outA, err, nullptr, {}};
    JobContext ctxB{inB, {}, outB, err, nullptr, {}};
    REQUIRE((*upper)(ctxA).has_value());
    REQUIRE((*upper)(ctxB).has_value());
    const Payload* preds[] = {&outA, &outB};
    JobContext ctxC{inC, preds, outC, err, nullptr, {}};
    REQUIRE((*concat)(ctxC).has_value());
    CHECK(outC == got.results[c].output);
}

TEST_CASE("Remote handshake: a fingerprint mismatch fails connect before any request")
{
    Rig rig;
    rig.startWorker();
    auto result = rig.connect({.expectedFingerprint = rig.registry.fingerprint() ^ 1U});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ConnectError::kFingerprintMismatch);
    CHECK(rig.channel->freeCapacity() == 0U);

    Pipeline pipe;
    auto remote = rig.channel->emplace(pipe, single("upper", "x"));
    auto run = pipe.runInline();
    REQUIRE_FALSE(run.has_value());
    CHECK(run.error() == PipelineError::kUnavailable);
    CHECK(pipe.status(remote) == JobStatus::kFailed);
}

TEST_CASE("Remote handshake: a different protocol major version is refused")
{
    Rig rig;
    rig.startWorker();

    struct Capture final : IFrameSink
    {
        std::mutex mtx;
        std::vector<std::vector<std::byte>> frames;
        std::atomic<bool> closed{false};
        void onFrame(std::span<const std::byte> frame) override
        {
            std::lock_guard lock{mtx};
            frames.emplace_back(frame.begin(), frame.end());
        }
        void onClosed(TransportError) noexcept override { closed = true; }
    } capture;
    rig.coordinatorEnd->start(capture);
    Hello hello;
    hello.major = static_cast<uint16_t>(kProtocolMajor + 1U);
    hello.fingerprint = rig.registry.fingerprint();
    hello.maxInFlight = 1U;
    REQUIRE(rig.coordinatorEnd->send(wire::encodeHello(FrameType::kHello, hello)).has_value());
    REQUIRE(waitUntil([&] { return capture.closed.load(); }));
    std::lock_guard lock{capture.mtx};
    REQUIRE(capture.frames.size() == 1U);
    CHECK(wire::frameType(capture.frames[0]) == FrameType::kHelloReject);
    auto reject = wire::decodeReject(capture.frames[0]);
    REQUIRE(reject.has_value());
    CHECK(reject->reason == RejectReason::kVersion);
}

TEST_CASE("Remote: an unknown kind fails that job by name, and the remote job with it")
{
    Rig rig;
    rig.startWorker();
    REQUIRE(rig.connect().has_value());

    struct Failures final : IObserver
    {
        std::string message;
        void onJobFailure(RunId, JobId, std::string_view, PipelineError, std::string_view m) noexcept override
        {
            message = std::string{m};
        }
    } failures;
    Pipeline pipe;
    SubGraphReply got;
    SubGraphRequest request;
    request.add({"missing", 3});
    auto remote = rig.channel->emplace(pipe, request, [&](const SubGraphReply& r) { got = r; }).name("remote");
    auto result = pipe.runInline(&failures);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
    CHECK(pipe.firstFailureName() == "remote");
    CHECK(pipe.status(remote) == JobStatus::kFailed);
    REQUIRE(got.results.size() == 1U);
    CHECK(got.results[0].status == JobStatus::kFailed);
    CHECK(got.results[0].message == "unknown job kind missing@3");
    CHECK(failures.message.find("missing@3") != std::string::npos);
}

TEST_CASE("Remote: per-job errors are reported, and the lowest failed index decides the remote job")
{
    Rig rig;
    rig.startWorker();
    REQUIRE(rig.connect().has_value());
    struct Failures final : IObserver
    {
        std::string message;
        void onJobFailure(RunId, JobId, std::string_view, PipelineError, std::string_view m) noexcept override
        {
            message = std::string{m};
        }
    } failures;

    SubGraphRequest request;
    request.add({"upper", 1}, Payload::inlineBytes(bytes("ok")));
    auto first = request.add({"fail", 1}, Payload::inlineBytes(bytes("one")));
    request.add({"fail", 1}, Payload::inlineBytes(bytes("two")));
    auto after = request.add({"upper", 1});
    request.edge(first, after);
    Pipeline pipe;
    SubGraphReply got;
    (void)rig.channel->emplace(pipe, request, [&](const SubGraphReply& r) { got = r; });
    REQUIRE_FALSE(pipe.runInline(&failures).has_value());
    REQUIRE(got.results.size() == 4U);
    CHECK(got.results[0].status == JobStatus::kDone);
    CHECK(got.results[1].message == "bad input one");
    CHECK(got.results[2].message == "bad input two");
    CHECK(got.results[3].status == JobStatus::kSkipped);
    CHECK(got.firstFailure() == 1U);
    CHECK(failures.message.find("bad input one") != std::string::npos);
}

TEST_CASE("Remote: worker loss fails every request in flight with kUnavailable")
{
    Rig rig;
    rig.startWorker();
    REQUIRE(rig.connect().has_value());

    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    auto r1 = rig.channel->emplace(pipe, single("block", ""));
    auto r2 = rig.channel->emplace(pipe, single("block", ""));
    std::jthread loss{[&] {
        if (waitUntil([&] { return rig.kinds.blocked.load() == 2; })) rig.workerEnd->close();
    }};
    auto result = pipe.run(pool);
    loss.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kUnavailable);
    CHECK(pipe.status(r1) == JobStatus::kFailed);
    CHECK(pipe.status(r2) == JobStatus::kFailed);
    CHECK(rig.channel->freeCapacity() == 0U);

    // Later requests fail at once.
    Pipeline again;
    (void)rig.channel->emplace(again, single("upper", "x"));
    auto later = again.runInline();
    REQUIRE_FALSE(later.has_value());
    CHECK(later.error() == PipelineError::kUnavailable);
}

TEST_CASE("Remote back-pressure: a full channel fails fast with kBusy")
{
    Rig rig;
    rig.startWorker({.maxConcurrentRequests = 1});
    REQUIRE(rig.connect({.maxInFlight = 4}).has_value());
    CHECK(rig.channel->freeCapacity() == 1U); // negotiated down to the worker's limit

    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    auto first = rig.channel->emplace(pipe, single("block", "")).name("first");
    auto second = rig.channel->emplace(pipe, single("upper", "x")).name("second");
    second.succeed(pipe.emplace([&] { (void)waitUntil([&] { return rig.kinds.blocked.load() == 1; }); }));
    std::atomic<uint32_t> capacityWhileFull{99U};
    std::jthread release{[&] {
        if (waitUntil([&] { return pipe.status(second) == JobStatus::kFailed; }))
            capacityWhileFull = rig.channel->freeCapacity();
        rig.kinds.released = true;
    }};
    auto result = pipe.run(pool);
    release.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kBusy);
    CHECK(pipe.firstFailureName() == "second");
    CHECK(pipe.status(first) == JobStatus::kDone);
    CHECK(capacityWhileFull.load() == 0U);
    CHECK(rig.channel->freeCapacity() == 1U);
}

TEST_CASE("Remote back-pressure: a bounded queue holds requests until a slot frees")
{
    Rig rig;
    rig.startWorker({.maxConcurrentRequests = 1});
    REQUIRE(rig.connect({.maxInFlight = 1, .queueCapacity = 2}).has_value());
    CHECK(rig.channel->freeCapacity() == 3U);

    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    std::vector<Job> remotes;
    for (int i = 0; i < 3; ++i) remotes.push_back(rig.channel->emplace(pipe, single("upper", "q")));
    REQUIRE(pipe.run(pool).has_value());
    for (auto job : remotes) CHECK(pipe.status(job) == JobStatus::kDone);
}

TEST_CASE("Remote: cancelling a remote job stops the worker's request; the late reply is dropped")
{
    Rig rig;
    rig.startWorker();
    REQUIRE(rig.connect().has_value());

    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    bool replied = false;
    auto remote = rig.channel->emplace(pipe, single("block", ""), [&](const SubGraphReply&) { replied = true; });
    std::stop_source stop;
    std::jthread canceller{[&] {
        if (waitUntil([&] { return rig.kinds.blocked.load() == 1; })) stop.request_stop();
    }};
    auto result = pipe.run(pool, stop.get_token());
    canceller.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCancelled);
    CHECK(pipe.status(remote) == JobStatus::kCancelled);
    CHECK(waitUntil([&] { return rig.kinds.stopsSeen.load() == 1; }));
    CHECK(waitUntil([&] { return !pipe.hasPendingOrphans(); }));
    CHECK_FALSE(replied);
    CHECK(rig.channel->freeCapacity() == rig.channel->peer().maxInFlight);
}

TEST_CASE("Remote: more awaiting remote jobs than coordinator threads while CPU jobs finish")
{
    Rig rig;
    rig.startWorker({.maxConcurrentRequests = 8});
    REQUIRE(rig.connect({.maxInFlight = 8}).has_value());

    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    std::atomic<int> cpuDone{0};
    for (int i = 0; i < 6; ++i) (void)rig.channel->emplace(pipe, single("block", ""));
    for (int i = 0; i < 6; ++i) (void)pipe.emplace([&] { ++cpuDone; });
    std::atomic<bool> cpuFinishedWhileWaiting{false};
    std::jthread release{[&] {
        cpuFinishedWhileWaiting = waitUntil([&] { return rig.kinds.blocked.load() == 6; })
                                  && waitUntil([&] { return cpuDone.load() == 6; });
        rig.kinds.released = true;
    }};
    REQUIRE(pipe.run(pool).has_value());
    release.join();
    CHECK(cpuFinishedWhileWaiting.load());
}

TEST_CASE("Remote payloads: large outputs come back as store handles; oversized inline inputs are refused")
{
    Rig rig;
    rig.startWorker({.maxInlineBytes = 1024});
    REQUIRE(rig.connect().has_value());

    Pipeline pipe;
    Payload output;
    (void)rig.channel->emplace(pipe, single("big", ""), [&](const SubGraphReply& r) { output = r.results[0].output; });
    REQUIRE(pipe.runInline().has_value());
    REQUIRE(output.kind() == PayloadKind::kHandle);
    auto resolved = rig.store.get(output.bytes());
    REQUIRE(resolved.has_value());
    CHECK(resolved->size() == 4096U);

    Pipeline tooLarge;
    SubGraphRequest request;
    request.add({"upper", 1}, Payload::inlineBytes(std::vector<std::byte>(2048U)));
    auto remote = rig.channel->emplace(tooLarge, request);
    auto result = tooLarge.runInline();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
    CHECK(tooLarge.status(remote) == JobStatus::kFailed);
}

TEST_CASE("Remote: a pipeline with remote jobs re-runs")
{
    Rig rig;
    rig.startWorker();
    REQUIRE(rig.connect().has_value());
    Pipeline pipe;
    int replies = 0;
    (void)rig.channel->emplace(pipe, single("upper", "r"), [&](const SubGraphReply&) { ++replies; });
    for (int i = 0; i < 5; ++i) REQUIRE(pipe.runInline().has_value());
    CHECK(replies == 5);
}
