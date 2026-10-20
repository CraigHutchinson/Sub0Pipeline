// tests/test_payload.cpp
//
// Payload transfer: views, the pipe transport, streaming at boundary sizes,
// strategy negotiation and fallback, the store strategy, consumer strategies
// and view lifetimes.

#include <atomic>
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
#include <thread>
#include <vector>

#include "sub0pipeline/remote/channel.hpp"
#include "sub0pipeline/remote/loopback_transport.hpp"
#include "sub0pipeline/remote/pipe_transport.hpp"
#include "sub0pipeline/remote/strategy.hpp"
#include "sub0pipeline/remote/worker_host.hpp"
#include "sub0pipeline/sub0pipeline.hpp"
#include "payload_test_helpers.hpp"

using namespace sub0pipeline;
using namespace sub0pipeline::remote;
using namespace std::chrono_literals;
using namespace payload_test;

namespace
{

class CountingStore final : public IPayloadStore
{
public:
    std::expected<std::vector<std::byte>, PipelineError> get(std::span<const std::byte> handle) override
    {
        std::lock_guard lock{mtx_};
        ++gets;
        auto found = items_.find(std::vector<std::byte>{handle.begin(), handle.end()});
        if (found == items_.end()) return std::unexpected(PipelineError::kJobFailed);
        return found->second;
    }
    std::expected<std::vector<std::byte>, PipelineError> put(std::span<const std::byte> bytes) override
    {
        std::lock_guard lock{mtx_};
        ++puts;
        std::vector<std::byte> key(8);
        for (std::size_t i = 0; i < 8; ++i) key[i] = static_cast<std::byte>((items_.size() >> (8U * i)) & 0xFFU);
        items_[key] = {bytes.begin(), bytes.end()};
        return key;
    }
    int puts = 0;
    int gets = 0;
private:
    std::mutex mtx_;
    std::map<std::vector<std::byte>, std::vector<std::byte>> items_;
};

// A consumer-defined strategy: sends bytes in its own frames, counted.
class TaggedStrategy final : public IPayloadStrategy
{
public:
    explicit TaggedStrategy(std::string name, bool acceptFails, std::atomic<int>* sends)
        : name_{std::move(name)}, acceptFails_{acceptFails}, sends_{sends} {}
    std::string_view name() const noexcept override { return name_; }
    std::vector<std::byte> offer() override { return {std::byte{1}}; }
    std::expected<std::vector<std::byte>, PipelineError> accept(std::span<const std::byte>) override
    {
        if (acceptFails_) return std::unexpected(PipelineError::kUnavailable);
        return std::vector<std::byte>{std::byte{2}};
    }
    std::expected<void, PipelineError> complete(std::span<const std::byte> accepted) override
    {
        if (accepted.size() != 1U || accepted[0] != std::byte{2}) return std::unexpected(PipelineError::kJobFailed);
        return {};
    }
    void attach(IPayloadSender& sender) override { sender_ = &sender; }
    std::expected<std::vector<std::byte>, PipelineError> send(const Payload& payload, uint64_t) override
    {
        if (sends_) ++*sends_;
        uint64_t id = nextId_++;
        std::byte idBytes[8];
        for (std::size_t i = 0; i < 8; ++i) idBytes[i] = static_cast<std::byte>((id >> (8U * i)) & 0xFFU);
        std::span<const std::byte> parts[] = {idBytes, payload.bytes()};
        if (!sender_->sendPayloadFrame(parts)) return std::unexpected(PipelineError::kUnavailable);
        return std::vector<std::byte>{idBytes, idBytes + 8};
    }
    std::expected<Payload, PipelineError> receive(std::span<const std::byte> reference) override
    {
        std::lock_guard lock{mtx_};
        auto found = received_.find(std::vector<std::byte>{reference.begin(), reference.end()});
        if (found == received_.end()) return std::unexpected(PipelineError::kJobFailed);
        auto payload = Payload::inlineBytes(std::move(found->second));
        received_.erase(found);
        return payload;
    }
    void onPayloadFrame(const FrameView& frame) override
    {
        std::lock_guard lock{mtx_};
        received_[std::vector<std::byte>{frame.body.begin(), frame.body.begin() + 8}] =
            std::vector<std::byte>{frame.body.begin() + 8, frame.body.end()};
    }
private:
    std::string name_;
    bool acceptFails_;
    std::atomic<int>* sends_;
    IPayloadSender* sender_{nullptr};
    std::atomic<uint64_t> nextId_{1U};
    std::mutex mtx_;
    std::map<std::vector<std::byte>, std::vector<std::byte>> received_;
};

PayloadStrategyFactory tagged(std::string name, bool acceptFails = false, std::atomic<int>* sends = nullptr)
{
    return [=] { return std::make_unique<TaggedStrategy>(name, acceptFails, sends); };
}

} // namespace

TEST_CASE("Payload views share their owner; copies keep the bytes alive")
{
    auto storage = std::make_shared<std::vector<std::byte>>(pattern(100));
    std::weak_ptr<std::vector<std::byte>> watch = storage;
    Payload copy;
    {
        auto view = Payload::view(*storage, storage);
        storage.reset();
        copy = view;
        CHECK(view.kind() == PayloadKind::kInline);
        CHECK(view == Payload::inlineBytes(pattern(100)));
    }
    CHECK_FALSE(watch.expired());
    CHECK(copy.bytes().size() == 100U);
    CHECK(copy == Payload::inlineBytes(pattern(100)));
    copy = {};
    CHECK(watch.expired());

    auto writable = makeWritablePayload(16);
    std::fill(writable.bytes.begin(), writable.bytes.end(), std::byte{0x42});
    CHECK(writable.payload.bytes().size() == 16U);
    CHECK(writable.payload.bytes()[15] == std::byte{0x42});
}

TEST_CASE("PipeTransport: frames and parted frames arrive in order; close reaches both sides")
{
    auto [a, b] = PipeTransport::pair();
    REQUIRE(a);
    REQUIRE(b);
    struct Sink final : IFrameSink
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
    } sinkA, sinkB;
    a->start(sinkA);
    b->start(sinkB);

    auto cancel = wire::encodeCancel(5U);
    REQUIRE(a->send(cancel).has_value());
    auto body = pattern(300000);
    auto header = wire::payloadFrameHeader(static_cast<uint32_t>(body.size()));
    std::span<const std::byte> parts[] = {header, body};
    REQUIRE(a->sendParts(parts).has_value());
    REQUIRE(waitFor([&] { std::lock_guard lock{sinkB.mtx}; return sinkB.frames.size() == 2U; }));
    {
        std::lock_guard lock{sinkB.mtx};
        CHECK(wire::decodeCancel(sinkB.frames[0]) == 5U);
        CHECK(wire::frameType(sinkB.frames[1]) == FrameType::kPayload);
        CHECK(std::equal(body.begin(), body.end(), sinkB.frames[1].begin() + 5));
    }
    a->close();
    CHECK(waitFor([&] { return sinkA.closed.load() && sinkB.closed.load(); }));
}

TEST_CASE("Streaming: payloads at chunk boundaries and 16 MiB round-trip over loopback and pipes")
{
    constexpr uint32_t kChunk = 4096U;
    for (auto medium : {Medium::kLoopback, Medium::kPipe})
    {
        CAPTURE(static_cast<int>(medium));
        Link link{medium};
        link.start({.maxInlineBytes = 0U, .strategies = {streamStrategy({.chunkBytes = kChunk})}},
                   {.maxInlineBytes = 0U, .strategies = {streamStrategy({.chunkBytes = kChunk})}});
        CHECK(link.channel->payloadStrategy() == "stream");
        for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{kChunk - 1U}, std::size_t{kChunk},
                                 std::size_t{kChunk + 1U}, std::size_t{16U * 1024U * 1024U}})
        {
            CAPTURE(size);
            auto output = link.echo(Payload::inlineBytes(pattern(size)));
            CHECK(output.kind() == PayloadKind::kInline);
            CHECK(output == Payload::inlineBytes(pattern(size)));
        }
        CHECK(link.badInputs.load() == 0);
    }
}

TEST_CASE("Streaming: concurrent requests interleave their chunks safely")
{
    Link link{Medium::kPipe};
    link.start({.maxConcurrentRequests = 8, .maxInlineBytes = 0U, .strategies = {streamStrategy({.chunkBytes = 65536U})}},
               {.maxInFlight = 8, .maxInlineBytes = 0U, .strategies = {streamStrategy({.chunkBytes = 65536U})}});
    PriorityExecutor pool{{.threadCount = 8}};
    Pipeline pipe;
    std::vector<Payload> outputs(8);
    for (std::size_t i = 0; i < 8; ++i)
    {
        SubGraphRequest request;
        request.add({"echo", 1}, Payload::inlineBytes(pattern(256U * 1024U)), true);
        (void)link.channel->emplace(pipe, request, [&outputs, i](const SubGraphReply& r) { outputs[i] = r.results[0].output; });
    }
    REQUIRE(pipe.run(pool).has_value());
    for (const auto& output : outputs) CHECK(output == Payload::inlineBytes(pattern(256U * 1024U)));
    CHECK(link.badInputs.load() == 0);
}

TEST_CASE("Negotiation: the coordinator's preference wins among strategies both sides support")
{
    CountingStore store;
    {
        Link link{Medium::kLoopback};
        link.start({.strategies = {storeStrategy(store)}}, {.strategies = {tagged("custom"), storeStrategy(store)}});
        CHECK(link.channel->payloadStrategy() == "store");
    }
    {
        Link link{Medium::kLoopback};
        link.start({.strategies = {tagged("custom"), storeStrategy(store)}},
                   {.strategies = {tagged("custom"), storeStrategy(store)}});
        CHECK(link.channel->payloadStrategy() == "custom");
    }
}

TEST_CASE("Negotiation: a worker without the preferred strategy falls back to streaming")
{
    CountingStore store;
    Link link{Medium::kLoopback};
    link.start({.maxInlineBytes = 1024U}, {.maxInlineBytes = 1024U, .strategies = {storeStrategy(store)}});
    CHECK(link.channel->payloadStrategy() == "stream");
    CHECK(link.echo(Payload::inlineBytes(pattern(100000))) == Payload::inlineBytes(pattern(100000)));
    CHECK(store.puts == 0);
}

TEST_CASE("Negotiation: a strategy the worker cannot accept is skipped for the next")
{
    std::atomic<int> sends{0};
    Link link{Medium::kLoopback};
    link.start({.maxInlineBytes = 1024U, .strategies = {tagged("fragile", true), tagged("sturdy", false, &sends)}},
               {.maxInlineBytes = 1024U, .strategies = {tagged("fragile", false), tagged("sturdy", false, &sends)}});
    CHECK(link.channel->payloadStrategy() == "sturdy");
    CHECK(link.echo(Payload::inlineBytes(pattern(5000))) == Payload::inlineBytes(pattern(5000)));
    CHECK(sends.load() == 2); // the input one way, the output back
}

TEST_CASE("Store strategy: large payloads travel through the store both ways")
{
    CountingStore store;
    Link link{Medium::kLoopback};
    link.start({.maxInlineBytes = 1024U, .strategies = {storeStrategy(store)}},
               {.maxInlineBytes = 1024U, .strategies = {storeStrategy(store)}});
    REQUIRE(link.channel->payloadStrategy() == "store");
    CHECK(link.echo(Payload::inlineBytes(pattern(1U << 20U))) == Payload::inlineBytes(pattern(1U << 20U)));
    CHECK(store.puts == 2);
    CHECK(store.gets == 2);
    CHECK(link.echo(Payload::inlineBytes(pattern(10))) == Payload::inlineBytes(pattern(10))); // inline, no store
    CHECK(store.puts == 2);
}

TEST_CASE("Views: a reply output stays valid after the channel and the worker are gone")
{
    Payload kept;
    {
        Link link{Medium::kPipe};
        link.start({.maxInlineBytes = 0U}, {.maxInlineBytes = 0U});
        auto input = link.channel->allocatePayload(1U << 20U);
        auto bytes = pattern(1U << 20U);
        std::copy(bytes.begin(), bytes.end(), input.bytes.begin());
        kept = link.echo(input.payload);
    }
    CHECK(kept == Payload::inlineBytes(pattern(1U << 20U)));
}
