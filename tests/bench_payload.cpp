// tests/bench_payload.cpp
//
// Payload transfer benchmarks (req-payload-baseline): each shipped payload
// strategy against a hand-written baseline that moves the same bytes over the
// same medium with no library code, for 64 KiB, 1 MiB and 16 MiB; and remote
// round trips per strategy. Run manually; not part of ctest.
#define ANKERL_NANOBENCH_IMPLEMENT
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <nanobench.h>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sub0pipeline/remote/channel.hpp"
#include "sub0pipeline/remote/loopback_transport.hpp"
#include "sub0pipeline/remote/pipe_transport.hpp"
#include "sub0pipeline/remote/shared_memory.hpp"
#include "sub0pipeline/remote/strategy.hpp"
#include "sub0pipeline/remote/worker_host.hpp"
#include "sub0pipeline/sub0pipeline.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace sub0pipeline;
using namespace sub0pipeline::remote;

namespace
{

constexpr std::size_t kSizes[] = {64U * 1024U, 1024U * 1024U, 16U * 1024U * 1024U};

std::string sizeLabel(std::size_t size)
{
    return size >= 1024U * 1024U ? std::to_string(size / (1024U * 1024U)) + " MiB"
                                 : std::to_string(size / 1024U) + " KiB";
}

// ── Raw pipe baseline: the same medium with no library code ─────────────────

class RawPipe
{
public:
    RawPipe()
    {
#if defined(_WIN32)
        HANDLE r = nullptr;
        HANDLE w = nullptr;
        CreatePipe(&r, &w, nullptr, 1U << 20U); // same buffer as PipeTransport::pair()
        read_ = r;
        write_ = w;
#else
        int fds[2];
        if (::pipe(fds) == 0)
        {
            read_ = fds[0];
            write_ = fds[1];
        }
#endif
    }
    ~RawPipe()
    {
#if defined(_WIN32)
        CloseHandle(read_);
        CloseHandle(write_);
#else
        ::close(read_);
        ::close(write_);
#endif
    }
    void writeAll(const std::byte* data, std::size_t size)
    {
        while (size > 0U)
        {
#if defined(_WIN32)
            DWORD n = 0;
            WriteFile(write_, data, static_cast<DWORD>(size), &n, nullptr);
#else
            const auto n = ::write(write_, data, size);
            if (n <= 0) return;
#endif
            data += n;
            size -= static_cast<std::size_t>(n);
        }
    }
    void readAll(std::byte* data, std::size_t size)
    {
        while (size > 0U)
        {
#if defined(_WIN32)
            DWORD n = 0;
            if (!ReadFile(read_, data, static_cast<DWORD>(size), &n, nullptr) || n == 0) return;
#else
            const auto n = ::read(read_, data, size);
            if (n <= 0) return;
#endif
            data += n;
            size -= static_cast<std::size_t>(n);
        }
    }

private:
#if defined(_WIN32)
    HANDLE read_{nullptr};
    HANDLE write_{nullptr};
#else
    int read_{-1};
    int write_{-1};
#endif
};

// ── Streaming over PipeTransport, driven without a channel ──────────────────

class BenchSender final : public IPayloadSender
{
public:
    explicit BenchSender(ITransport& transport) : transport_{transport} {}
    std::expected<void, TransportError> sendPayloadFrame(std::span<const std::span<const std::byte>> parts) override
    {
        std::size_t body = 0U;
        for (auto p : parts) body += p.size();
        const auto header = wire::payloadFrameHeader(static_cast<uint32_t>(body));
        std::span<const std::byte> all[4] = {header};
        for (std::size_t i = 0; i < parts.size(); ++i) all[i + 1] = parts[i];
        return transport_.sendParts(std::span<const std::span<const std::byte>>{all, parts.size() + 1U});
    }
private:
    ITransport& transport_;
};

// Routes payload frames to a strategy and signals when @p expected chunk bytes arrived.
class CountingSink final : public IFrameSink
{
public:
    explicit CountingSink(IPayloadStrategy& strategy) : strategy_{strategy} {}
    void expect(std::size_t bytes) { expected_ = bytes; received_ = 0U; }
    void onFrame(std::span<const std::byte> frame) override { onFrameBuffer(nullptr, frame); }
    void onFrameBuffer(std::shared_ptr<const void> owner, std::span<const std::byte> frame) override
    {
        const auto body = frame.subspan(FrameReader::kHeaderBytes);
        strategy_.onPayloadFrame({body, std::move(owner)});
        received_ += body.size() - 25U; // the stream chunk header
        if (received_ >= expected_) done.release();
    }
    void onClosed(TransportError) noexcept override {}
    std::binary_semaphore done{0};
private:
    IPayloadStrategy& strategy_;
    std::size_t expected_{0U};
    std::size_t received_{0U};
};

class MemoryStore final : public IPayloadStore
{
public:
    std::expected<std::vector<std::byte>, PipelineError> get(std::span<const std::byte> handle) override
    {
        std::lock_guard lock{mtx_};
        return items_.at(std::vector<std::byte>{handle.begin(), handle.end()});
    }
    std::expected<std::vector<std::byte>, PipelineError> put(std::span<const std::byte> bytes) override
    {
        std::lock_guard lock{mtx_};
        std::vector<std::byte> key{std::byte{1}};
        items_[key] = {bytes.begin(), bytes.end()};
        return key;
    }
private:
    std::mutex mtx_;
    std::map<std::vector<std::byte>, std::vector<std::byte>> items_;
};

// ── Raw shared mapping baseline ─────────────────────────────────────────────

class RawMapping
{
public:
    explicit RawMapping(std::size_t size) : size_{size}
    {
#if defined(_WIN32)
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      static_cast<DWORD>(static_cast<unsigned long long>(size) >> 32U),
                                      static_cast<DWORD>(size & 0xFFFFFFFFULL), nullptr);
        data_ = static_cast<std::byte*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, size));
#else
        data_ = static_cast<std::byte*>(::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
#endif
    }
    ~RawMapping()
    {
#if defined(_WIN32)
        UnmapViewOfFile(data_);
        CloseHandle(mapping_);
#else
        ::munmap(data_, size_);
#endif
    }
    std::byte* data() const { return data_; }
private:
    std::size_t size_;
    std::byte*  data_{nullptr};
#if defined(_WIN32)
    HANDLE mapping_{nullptr};
#endif
};

class NullSender final : public IPayloadSender
{
public:
    std::expected<void, TransportError> sendPayloadFrame(std::span<const std::span<const std::byte>>) override { return {}; }
};

void configure(ankerl::nanobench::Bench& bench, std::size_t size)
{
    bench.warmup(3).minEpochIterations(size >= 16U * 1024U * 1024U ? 5U : 50U).epochs(7).relative(false);
}

void transferBenchmarks()
{
    // Stream over pipes vs raw pipe write/read.
    for (auto size : kSizes)
    {
        ankerl::nanobench::Bench bench;
        bench.title("stream over pipe, " + sizeLabel(size)).unit("transfer");
        configure(bench, size);
        std::vector<std::byte> source(size, std::byte{0x5A});

        {
            RawPipe pipe;
            std::vector<std::byte> target(size);
            std::counting_semaphore<> go{0};
            std::binary_semaphore done{0};
            std::atomic<bool> stop{false};
            std::jthread reader{[&] {
                for (;;)
                {
                    go.acquire();
                    if (stop) return;
                    pipe.readAll(target.data(), size);
                    done.release();
                }
            }};
            bench.run("baseline: raw pipe write + read into a buffer", [&] {
                go.release();
                pipe.writeAll(source.data(), size);
                done.acquire();
            });
            stop = true;
            go.release();
        }

        {
            auto [a, b] = PipeTransport::pair();
            auto sending = streamStrategy()();
            auto receiving = streamStrategy()();
            auto accepted = receiving->accept(sending->offer());
            (void)sending->complete(*accepted);
            BenchSender sender{*a};
            sending->attach(sender);
            CountingSink sink{*receiving};
            struct Idle final : IFrameSink
            {
                void onFrame(std::span<const std::byte>) override {}
                void onClosed(TransportError) noexcept override {}
            } idle;
            a->start(idle);
            b->start(sink);
            const auto payload = Payload::inlineBytes(source);
            bench.run("strategy: stream send + receive (view of the frame buffer)", [&] {
                sink.expect(size);
                auto reference = sending->send(payload, 1U);
                sink.done.acquire();
                auto received = receiving->receive(*reference);
                ankerl::nanobench::doNotOptimizeAway(received->bytes().data());
            });
            a->close();
        }
    }

    // Shared memory vs raw memcpy into a shared mapping of the same size.
    for (auto size : kSizes)
    {
        ankerl::nanobench::Bench bench;
        bench.title("shm, " + sizeLabel(size)).unit("transfer");
        configure(bench, size);
        std::vector<std::byte> source(size, std::byte{0x5A});
        {
            RawMapping mapping{size};
            bench.run("baseline: raw memcpy into a shared mapping", [&] {
                std::memcpy(mapping.data(), source.data(), size);
                ankerl::nanobench::doNotOptimizeAway(mapping.data()[size - 1U]);
            });
        }
        const SharedMemoryOptions options{.regionBytes = 2U * size};
        SharedMemoryStrategy coordinator{options};
        SharedMemoryStrategy worker{options};
        auto accepted = worker.accept(coordinator.offer());
        (void)coordinator.complete(*accepted);
        NullSender sender;
        coordinator.attach(sender);
        worker.attach(sender);
        // A view of the baseline's own source, so both copy from the same bytes.
        const auto payload = Payload::view(source, std::shared_ptr<const void>{&source, [](const void*) {}});
        uint64_t scope = 0U;
        bench.run("strategy: shm send (one memcpy) + receive (view)", [&] {
            ++scope;
            auto reference = coordinator.send(payload, scope);
            {
                auto received = worker.receive(*reference);
                ankerl::nanobench::doNotOptimizeAway(received->bytes().data());
            }
            coordinator.endScope(scope); // the reply would do this
        });
        auto inPlace = coordinator.allocate(size);
        std::memcpy(inPlace->bytes.data(), source.data(), size);
        bench.run("strategy: shm send of an in-place buffer (no copy) + receive", [&] {
            ++scope;
            auto reference = coordinator.send(inPlace->payload, scope);
            {
                auto received = worker.receive(*reference);
                ankerl::nanobench::doNotOptimizeAway(received->bytes().data());
            }
            coordinator.endScope(scope);
        });
    }

    // Store strategy vs raw put + get on the same store.
    for (auto size : kSizes)
    {
        ankerl::nanobench::Bench bench;
        bench.title("store, " + sizeLabel(size)).unit("transfer");
        configure(bench, size);
        std::vector<std::byte> source(size, std::byte{0x5A});
        MemoryStore store;
        bench.run("baseline: raw put + get", [&] {
            auto handle = store.put(source);
            auto bytes = store.get(*handle);
            ankerl::nanobench::doNotOptimizeAway(bytes->data());
        });
        auto strategy = storeStrategy(store)();
        const auto payload = Payload::inlineBytes(source);
        bench.run("strategy: store send + receive", [&] {
            auto reference = strategy->send(payload, 1U);
            auto received = strategy->receive(*reference);
            ankerl::nanobench::doNotOptimizeAway(received->bytes().data());
        });
    }
}

// ── Round trips per strategy ────────────────────────────────────────────────

struct Endpoint
{
    std::unique_ptr<ITransport> coordinatorEnd;
    std::unique_ptr<ITransport> workerEnd;
    PriorityExecutor            pool{{.threadCount = 4}};
    JobRegistry                 registry;
    std::optional<WorkerHost>   host;
    std::jthread                serving;
    std::optional<RemoteChannel> channel;

    Endpoint(bool pipe, const std::vector<PayloadStrategyFactory>& strategies)
    {
        if (pipe)
        {
            auto [a, b] = PipeTransport::pair();
            coordinatorEnd = std::move(a);
            workerEnd = std::move(b);
        }
        else
        {
            auto [a, b] = LoopbackTransport::pair();
            coordinatorEnd = std::move(a);
            workerEnd = std::move(b);
        }
        registry.add({"noop", 1}, [](JobContext&) -> std::expected<void, PipelineError> { return {}; });
        registry.add({"echo", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
            const auto in = ctx.input().bytes();
            auto out = ctx.allocateOutput(in.size());
            std::memcpy(out.data(), in.data(), in.size());
            return {};
        });
        host.emplace(registry, *workerEnd,
                     WorkerHost::Options{.executor = &pool, .maxConcurrentRequests = 1, .strategies = strategies});
        serving = std::jthread{[this](std::stop_token stop) { host->serve(stop); }};
        channel.emplace(*coordinatorEnd,
                        RemoteChannel::Options{.expectedFingerprint = registry.fingerprint(), .strategies = strategies});
        (void)channel->connect();
    }
    ~Endpoint()
    {
        channel.reset();
        serving.request_stop();
        serving.join();
        host.reset();
    }
};

void roundTripBenchmarks()
{
    MemoryStore store;
    struct Variant { const char* name; bool pipe; std::vector<PayloadStrategyFactory> strategies; };
    const Variant variants[] = {
        {"loopback, stream", false, {}},
        {"pipe, stream", true, {}},
        {"pipe, store", true, {storeStrategy(store)}},
        {"loopback, shm", false, {sharedMemoryStrategy({.regionBytes = 64U * 1024U * 1024U})}},
        {"pipe, shm", true, {sharedMemoryStrategy({.regionBytes = 64U * 1024U * 1024U})}},
    };
    for (const auto& variant : variants)
    {
        Endpoint endpoint{variant.pipe, variant.strategies};
        SequentialExecutor sequential;
        ankerl::nanobench::Bench bench;
        bench.title(std::string{"round trip, "} + variant.name + " (" + endpoint.channel->payloadStrategy() + ")")
            .unit("request").warmup(3).minEpochIterations(20).epochs(7);
        for (int jobs : {1, 16, 256})
        {
            SubGraphRequest request;
            for (int i = 0; i < jobs; ++i) request.add({"noop", 1});
            Pipeline pipeline;
            (void)endpoint.channel->emplace(pipeline, request);
            bench.run(std::to_string(jobs) + "-job sub-graph, no payload", [&] { (void)pipeline.run(sequential); });
        }
        for (auto size : {std::size_t{1024U * 1024U}, std::size_t{16U * 1024U * 1024U}})
        {
            SubGraphRequest request;
            request.add({"echo", 1}, Payload::inlineBytes(std::vector<std::byte>(size, std::byte{1})), true);
            Pipeline pipeline;
            (void)endpoint.channel->emplace(pipeline, request);
            bench.run("1-job echo, " + sizeLabel(size) + " each way", [&] { (void)pipeline.run(sequential); });
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::string_view only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "transfer") transferBenchmarks();
    if (only.empty() || only == "roundtrip") roundTripBenchmarks();
    return 0;
}
