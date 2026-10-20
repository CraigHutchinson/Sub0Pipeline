// examples/remote_subgraph/main.cpp
//
// Demonstrates Sub0Pipeline::Remote:
// - A worker registers job kinds by name and version; no code travels.
// - The coordinator sends a three-job sub-graph (two parallel jobs feeding a
//   third) as one request; in its own graph that request is one deferred job.
// - The reply carries only the exported output, which a later local job uses.
// - A failing job is reported by name.
// - A 4 MiB payload goes to the worker and back through shared memory, which
//   both sides offer; a worker that could not map it would fall back to
//   streaming over the pipe. Both sides write their payloads in place.
//
// Both ends run in this process over PipeTransport::pair(). In a real
// deployment the worker is another process -- for example one hosting a
// runtime the coordinator does not have -- and each side wraps that process's
// pipes or socket in a PipeTransport.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sub0pipeline/remote/channel.hpp"
#include "sub0pipeline/remote/pipe_transport.hpp"
#include "sub0pipeline/remote/shared_memory.hpp"
#include "sub0pipeline/remote/worker_host.hpp"
#include "sub0pipeline/sub0pipeline.hpp"

using namespace sub0pipeline;
using namespace sub0pipeline::remote;

namespace
{

std::vector<std::byte> toBytes(std::string_view text)
{
    std::vector<std::byte> out(text.size());
    std::transform(text.begin(), text.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

std::string toText(std::span<const std::byte> data)
{
    std::string out(data.size(), '\0');
    std::transform(data.begin(), data.end(), out.begin(), [](std::byte b) { return static_cast<char>(b); });
    return out;
}

JobRegistry makeRegistry()
{
    JobRegistry registry;
    registry.add({"upper", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
        auto text = toText(ctx.input().bytes());
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        ctx.setOutput(Payload::inlineBytes(toBytes(text)));
        return {};
    });
    registry.add({"join", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
        std::string text;
        for (std::size_t i = 0; i < ctx.predecessorCount(); ++i)
        {
            if (i != 0) text += ' ';
            text += toText(ctx.predecessor(i).bytes());
        }
        ctx.setOutput(Payload::inlineBytes(toBytes(text)));
        return {};
    });
    registry.add({"invert", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
        const auto in = ctx.input().bytes();
        auto out = ctx.allocateOutput(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) out[i] = ~in[i];
        return {};
    });
    registry.add({"reject", 1}, [](JobContext& ctx) -> std::expected<void, PipelineError> {
        ctx.setError("input is not accepted");
        return std::unexpected(PipelineError::kJobFailed);
    });
    return registry;
}

} // namespace

int main()
{
    const JobRegistry registry = makeRegistry();
    auto [coordinatorEnd, workerEnd] = PipeTransport::pair();
    if (!coordinatorEnd || !workerEnd) return 1;

    // ── Worker side ──────────────────────────────────────────────────────────
    PriorityExecutor workerPool{{.threadCount = 2}};
    WorkerHost host{registry, *workerEnd,
                    {.executor = &workerPool, .maxConcurrentRequests = 2, .strategies = {sharedMemoryStrategy()}}};
    std::jthread serving{[&host](std::stop_token stop) { host.serve(stop); }};

    {
        // ── Coordinator side ─────────────────────────────────────────────────
        RemoteChannel channel{*coordinatorEnd, {.expectedFingerprint = registry.fingerprint(),
                                                .strategies = {sharedMemoryStrategy()}}};
        if (auto connected = channel.connect(); !connected)
        {
            std::printf("connect failed (%d)\n", static_cast<int>(connected.error()));
            return 1;
        }

        SubGraphRequest request;
        auto hello = request.add({"upper", 1}, Payload::inlineBytes(toBytes("hello")));
        auto world = request.add({"upper", 1}, Payload::inlineBytes(toBytes("world")));
        auto joined = request.add({"join", 1}, {}, /*exported=*/true);
        request.edge(hello, joined);
        request.edge(world, joined);

        Pipeline pipe;
        std::string greeting;
        auto remote = channel.emplace(pipe, request, [&](const SubGraphReply& reply) {
            greeting = toText(reply.results[joined].output.bytes());
        }).name("remote-greeting");
        (void)pipe.emplace([&] { std::printf("  [local] remote said: %s\n", greeting.c_str()); })
            .name("print").succeed(remote);

        PriorityExecutor pool{{.threadCount = 2}};
        const auto ok = pipe.run(pool);
        std::printf("Run 1: %s\n", ok ? "ok" : "failed");

        SubGraphRequest rejected;
        rejected.add({"reject", 1});
        Pipeline failing;
        (void)channel.emplace(failing, rejected).name("remote-reject");
        struct Report final : IObserver
        {
            void onJobFailure(RunId, JobId, std::string_view job, PipelineError, std::string_view message) noexcept override
            {
                std::printf("  [local] %.*s failed: %.*s\n", static_cast<int>(job.size()), job.data(),
                            static_cast<int>(message.size()), message.data());
            }
        } report;
        const auto failed = failing.run(pool, &report);
        std::printf("Run 2: %s\n", failed ? "unexpected success" : "failed as expected");

        // Above the inline limit (64 KiB by default) payloads go through the
        // negotiated strategy: here shared memory, so neither side copies them.
        constexpr std::size_t kImageBytes = 4U * 1024U * 1024U;
        auto image = channel.allocatePayload(kImageBytes);
        std::fill(image.bytes.begin(), image.bytes.end(), std::byte{0x0F});
        SubGraphRequest large;
        large.add({"invert", 1}, image.payload, /*exported=*/true);
        Pipeline big;
        Payload inverted;
        (void)channel.emplace(big, large, [&](const SubGraphReply& r) { inverted = r.results[0].output; });
        const auto streamed = big.run(pool);
        const bool correct = inverted.bytes().size() == kImageBytes && inverted.bytes()[0] == std::byte{0xF0};
        std::printf("Run 3: %s via \"%s\", 4 MiB returned %s\n", streamed ? "ok" : "failed",
                    channel.payloadStrategy().c_str(), correct ? "correctly" : "wrongly");
    } // the channel closes the connection; the worker's serve() then returns

    return 0;
}
