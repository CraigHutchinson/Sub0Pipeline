#pragma once
// tests/payload_test_helpers.hpp
//
// Shared by the payload-transfer suites: byte patterns, bounded waits, and a
// channel joined to a worker host over a chosen medium.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <doctest.h>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include "sub0pipeline/remote/channel.hpp"
#include "sub0pipeline/remote/loopback_transport.hpp"
#include "sub0pipeline/remote/pipe_transport.hpp"
#include "sub0pipeline/remote/worker_host.hpp"
#include "sub0pipeline/sub0pipeline.hpp"

namespace payload_test
{

using namespace sub0pipeline;
using namespace sub0pipeline::remote;
using namespace std::chrono_literals;

// Bounded so a broken implementation fails the check instead of hanging the suite.
inline bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds limit = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

inline std::vector<std::byte> pattern(std::size_t size, uint8_t seed = 7U)
{
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::byte>((i * 131U + seed) % 251U);
    return out;
}

enum class Medium { kLoopback, kPipe };

// A channel and a worker host joined by the chosen medium. The "echo" kind
// copies its input into an in-place output and checks its input view.
struct Link
{
    std::unique_ptr<ITransport> coordinatorEnd;
    std::unique_ptr<ITransport> workerEnd;
    PriorityExecutor            workerPool{{.threadCount = 4}};
    JobRegistry                 registry;
    std::atomic<int>            badInputs{0};
    std::optional<WorkerHost>   host;
    std::jthread                serving;
    std::optional<RemoteChannel> channel;

    explicit Link(Medium medium)
    {
        if (medium == Medium::kLoopback)
        {
            auto [a, b] = LoopbackTransport::pair();
            coordinatorEnd = std::move(a);
            workerEnd = std::move(b);
        }
        else
        {
            auto [a, b] = PipeTransport::pair();
            coordinatorEnd = std::move(a);
            workerEnd = std::move(b);
        }
        REQUIRE(coordinatorEnd);
        REQUIRE(workerEnd);
        registry.add({"echo", 1}, [this](JobContext& ctx) -> std::expected<void, PipelineError> {
            const auto in = ctx.input().bytes();
            const auto expected = pattern(in.size());
            if (!std::equal(in.begin(), in.end(), expected.begin(), expected.end())) ++badInputs;
            auto out = ctx.allocateOutput(in.size());
            std::copy(in.begin(), in.end(), out.begin());
            return {};
        });
    }

    void start(WorkerHost::Options workerOptions, RemoteChannel::Options channelOptions)
    {
        workerOptions.executor = &workerPool;
        host.emplace(registry, *workerEnd, workerOptions);
        serving = std::jthread{[this](std::stop_token stop) { host->serve(stop); }};
        channelOptions.expectedFingerprint = registry.fingerprint();
        channel.emplace(*coordinatorEnd, channelOptions);
        REQUIRE(channel->connect().has_value());
    }

    // Sends one echo request and returns its output.
    Payload echo(const Payload& input)
    {
        SubGraphRequest request;
        request.add({"echo", 1}, input, true);
        Pipeline pipe;
        Payload output;
        (void)channel->emplace(pipe, request, [&](const SubGraphReply& reply) { output = reply.results.at(0).output; });
        REQUIRE(pipe.runInline().has_value());
        return output;
    }

    ~Link()
    {
        channel.reset();
        if (serving.joinable())
        {
            serving.request_stop();
            serving.join();
        }
        host.reset();
    }
};


} // namespace payload_test
