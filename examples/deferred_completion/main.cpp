// examples/deferred_completion/main.cpp
//
// Demonstrates emplaceDeferred() and Completion:
// - A deferred job starts work on an asynchronous device and returns at once.
// - The device completes each request from its own thread; dependants start
//   only then, and no pool thread waits in the meantime.
// - Six transfers wait on a two-thread pool while CPU jobs keep running.
// - A request the device drops times out; its late completion is ignored, and
//   joinOrphans() waits for it.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include "sub0pipeline/sub0pipeline.hpp"

using namespace sub0pipeline;
using namespace std::chrono_literals;

namespace
{

// Serves requests one at a time on its own thread, like a DMA engine or a
// network peer. A dropped request is answered only when the device stops.
class AsyncDevice
{
public:
    AsyncDevice() : thread_{[this](std::stop_token stop) { serve(stop); }} {}

    void submit(int id, bool drop, Completion done)
    {
        {
            std::lock_guard lock{mtx_};
            queue_.push_back({id, drop, std::move(done)});
        }
        wake_.notify_one();
    }

    /** Stops serving; answers every dropped request on the way out. */
    void stop()
    {
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    struct Request
    {
        int        id{0};
        bool       drop{false};
        Completion done;
    };

    void serve(std::stop_token stop)
    {
        std::deque<Request> dropped;
        while (true)
        {
            Request request;
            {
                std::unique_lock lock{mtx_};
                if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) break;
                request = std::move(queue_.front());
                queue_.pop_front();
            }
            if (request.drop)
            {
                dropped.push_back(std::move(request));
                continue;
            }
            std::this_thread::sleep_for(20ms);
            std::printf("  [device] transfer %d done\n", request.id);
            request.done.succeed();
        }
        // Late answers to dropped requests: ignored by the pipeline.
        for (auto& request : dropped) request.done.succeed();
    }

    std::mutex                  mtx_;
    std::condition_variable_any wake_;
    std::deque<Request>         queue_;
    std::jthread                thread_; // last: stops before the state it uses
};

} // namespace

int main()
{
    PriorityExecutor pool{{.threadCount = 2}};
    AsyncDevice device;

    // ── Many transfers in flight on a small pool ─────────────────────────────
    {
        Pipeline pipe;
        auto report = pipe.emplace([] { std::printf("  [cpu] all transfers reported\n"); }).name("report");
        for (int i = 0; i < 6; ++i)
        {
            auto transfer = pipe.emplaceDeferred([&device, i](Completion done) {
                device.submit(i, false, std::move(done));
            });
            report.succeed(transfer);
        }
        for (int i = 0; i < 4; ++i)
        {
            (void)pipe.emplace([i] { std::printf("  [cpu] checksum block %d\n", i); });
        }
        const auto result = pipe.run(pool);
        std::printf("Run 1: %s\n\n", result ? "ok" : "failed");
    }

    // ── A dropped request times out ──────────────────────────────────────────
    {
        AsyncDevice flaky;
        Pipeline pipe;
        auto lost = pipe.emplaceDeferred([&flaky](Completion done) {
            flaky.submit(99, true, std::move(done));
        }).name("lost-transfer").timeout(100ms);
        (void)pipe.emplace([] { std::printf("  never runs\n"); }).succeed(lost);

        const auto result = pipe.run(pool);
        const auto failed = pipe.firstFailureName();
        std::printf("Run 2: %s in %.*s, orphan pending: %s\n",
                    !result && result.error() == PipelineError::kTimeout ? "timed out" : "unexpected",
                    static_cast<int>(failed.size()), failed.data(),
                    pipe.hasPendingOrphans() ? "yes" : "no");

        // Run 2 returned while the device still holds the Completion. Stopping
        // the device answers it late; joinOrphans() then returns at once.
        flaky.stop();
        pipe.joinOrphans();
        std::printf("After stop: orphan pending: %s\n", pipe.hasPendingOrphans() ? "yes" : "no");
    }
    return 0;
}
