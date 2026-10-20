// tests/test_shared_memory.cpp
//
// The "shm" payload strategy: round trips at boundary sizes, negotiation and
// fallback, cleanup when a peer dies, concurrent requests sharing a region,
// and view lifetimes.

#include <atomic>
#include <cstddef>
#include <doctest.h>
#include <memory>
#include <string>
#include <vector>

#include "payload_test_helpers.hpp"
#include "sub0pipeline/remote/shared_memory.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace payload_test;

namespace
{

// Records the instance each side's factory created, for inspection.
struct Observed
{
    std::atomic<SharedMemoryStrategy*> strategy{nullptr};
};

PayloadStrategyFactory observedShm(Observed& observed, SharedMemoryOptions options = {})
{
    return [&observed, options] {
        auto strategy = std::make_unique<SharedMemoryStrategy>(options);
        observed.strategy = strategy.get();
        return strategy;
    };
}

bool regionNameExists(const std::string& name)
{
#if defined(_WIN32)
    const std::wstring wide(name.begin(), name.end());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, wide.c_str());
    if (!mapping) return false;
    CloseHandle(mapping);
    return true;
#else
    const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) return false;
    ::close(fd);
    return true;
#endif
}

constexpr SharedMemoryOptions kSmallRegion{.regionBytes = 4U * 1024U * 1024U, .fallback = {.chunkBytes = 65536U}};

} // namespace

TEST_CASE("Shared memory: payloads round-trip at boundary sizes, and larger than the region via the fallback")
{
    for (auto medium : {Medium::kLoopback, Medium::kPipe})
    {
        CAPTURE(static_cast<int>(medium));
        Observed coordinator, worker;
        Link link{medium};
        link.start({.maxInlineBytes = 0U, .strategies = {observedShm(worker, kSmallRegion)}},
                   {.maxInlineBytes = 0U, .strategies = {observedShm(coordinator, kSmallRegion)}});
        REQUIRE(link.channel->payloadStrategy() == "shm");
        for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{65},
                                 std::size_t{1U << 20U}, std::size_t{4U * 1024U * 1024U}, std::size_t{16U * 1024U * 1024U}})
        {
            CAPTURE(size);
            auto output = link.echo(Payload::inlineBytes(pattern(size)));
            CHECK(output == Payload::inlineBytes(pattern(size)));
        }
        CHECK(link.badInputs.load() == 0);
        CHECK(waitFor([&] { return worker.strategy.load()->bytesInUse() == 0U; }));
        CHECK(coordinator.strategy.load()->bytesInUse() == 0U);
    }
}

TEST_CASE("Shared memory: in-place buffers are sent without a copy and freed when released")
{
    Observed coordinator, worker;
    Link link{Medium::kPipe};
    link.start({.maxInlineBytes = 0U, .strategies = {observedShm(worker, kSmallRegion)}},
               {.maxInlineBytes = 0U, .strategies = {observedShm(coordinator, kSmallRegion)}});
    REQUIRE(link.channel->payloadStrategy() == "shm");

    auto input = link.channel->allocatePayload(1U << 20U);
    CHECK(coordinator.strategy.load()->bytesInUse() >= (1U << 20U));
    auto bytes = pattern(1U << 20U);
    std::copy(bytes.begin(), bytes.end(), input.bytes.begin());
    {
        auto output = link.echo(input.payload);
        CHECK(output == Payload::inlineBytes(bytes));
        // The output is a view of the worker's region until this copy goes.
        CHECK(worker.strategy.load()->bytesInUse() >= (1U << 20U));
    }
    CHECK(waitFor([&] { return worker.strategy.load()->bytesInUse() == 0U; }));
    // The coordinator's input is still held by `input`; the worker finished with it.
    CHECK(coordinator.strategy.load()->bytesInUse() >= (1U << 20U));
    input = {};
    CHECK(coordinator.strategy.load()->bytesInUse() == 0U);
}

TEST_CASE("Shared memory: concurrent requests share the regions")
{
    Observed coordinator, worker;
    Link link{Medium::kPipe};
    link.start({.maxConcurrentRequests = 8, .maxInlineBytes = 0U, .strategies = {observedShm(worker, kSmallRegion)}},
               {.maxInFlight = 8, .maxInlineBytes = 0U, .strategies = {observedShm(coordinator, kSmallRegion)}});
    REQUIRE(link.channel->payloadStrategy() == "shm");
    PriorityExecutor pool{{.threadCount = 8}};
    {
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
    CHECK(waitFor([&] { return worker.strategy.load()->bytesInUse() == 0U; }));
    CHECK(coordinator.strategy.load()->bytesInUse() == 0U);
}

TEST_CASE("Shared memory: a worker that dies frees the coordinator's inputs; its outputs stay readable")
{
    Observed coordinator, worker;
    auto link = std::make_unique<Link>(Medium::kPipe);
    std::atomic<int> started{0};
    link->registry.add({"hang", 1}, [&started](JobContext& ctx) -> std::expected<void, PipelineError> {
        ++started;
        while (!ctx.stopToken().stop_requested()) std::this_thread::sleep_for(1ms);
        return std::unexpected(PipelineError::kCancelled);
    });
    link->start({.maxInlineBytes = 0U, .strategies = {observedShm(worker, kSmallRegion)}},
                {.maxInlineBytes = 0U, .strategies = {observedShm(coordinator, kSmallRegion)}});
    REQUIRE(link->channel->payloadStrategy() == "shm");
    const auto name = coordinator.strategy.load()->regionName();

    Payload kept = link->echo(Payload::inlineBytes(pattern(100000)));

    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    SubGraphRequest request;
    request.add({"hang", 1}, Payload::inlineBytes(pattern(500000)));
    auto remote = link->channel->emplace(pipe, request);
    std::jthread killer{[&] {
        if (waitFor([&] { return started.load() == 1; })) link->workerEnd->close();
    }};
    auto result = pipe.run(pool);
    killer.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kUnavailable);
    CHECK(pipe.status(remote) == JobStatus::kFailed);
    CHECK(coordinator.strategy.load()->bytesInUse() == 0U);

    link.reset(); // the worker host, its strategy and its region are gone
    CHECK(kept == Payload::inlineBytes(pattern(100000)));
#if !defined(_WIN32)
    CHECK_FALSE(regionNameExists(name)); // unlinked at the handshake
#else
    CHECK_FALSE(regionNameExists(name)); // freed with the last handle
#endif
}

TEST_CASE("Shared memory: a coordinator that goes away frees the worker's unreleased outputs")
{
    Observed coordinator, worker;
    Link link{Medium::kLoopback};
    link.start({.maxInlineBytes = 0U, .strategies = {observedShm(worker, kSmallRegion)}},
               {.maxInlineBytes = 0U, .strategies = {observedShm(coordinator, kSmallRegion)}});
    REQUIRE(link.channel->payloadStrategy() == "shm");
    Payload held = link.echo(Payload::inlineBytes(pattern(300000)));
    CHECK(worker.strategy.load()->bytesInUse() >= 300000U);
    link.channel.reset(); // closes the connection while the output is still held
    CHECK(waitFor([&] { return worker.strategy.load()->bytesInUse() == 0U; }));
    CHECK(held == Payload::inlineBytes(pattern(300000)));
}

TEST_CASE("Shared memory: a worker without shared memory falls back to streaming")
{
    Observed coordinator;
    Link link{Medium::kPipe};
    link.start({.maxInlineBytes = 1024U}, {.maxInlineBytes = 1024U, .strategies = {observedShm(coordinator, kSmallRegion)}});
    CHECK(link.channel->payloadStrategy() == "stream");
    CHECK(link.echo(Payload::inlineBytes(pattern(200000))) == Payload::inlineBytes(pattern(200000)));
}
