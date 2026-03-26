// examples/parallel_tasks/main.cpp
//
// Fan-out + fan-in pattern: one root dispatches N independent workers,
// all of which must complete before a final sink runs.
//
// Demonstrates real parallelism with DesktopExecutor and measures wall-clock
// speedup versus sequential execution.

#include <sub0pipeline/sub0pipeline.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace sub0pipeline { std::unique_ptr<IExecutor> makeDesktopExecutor(); }

using namespace sub0pipeline;
using namespace std::chrono_literals;

int main()
{
    constexpr int kWorkers = 8;
    constexpr auto kWorkDuration = 50ms;

    Pipeline pipeline;
    std::atomic<int> workersDone{0};

    // Root: single mandatory prerequisite.
    auto root = pipeline.emplace([]
    {
        std::printf("  [root] started\n");
    }).name("root");

    // Workers: independent, all succeed root.
    std::vector<Job> workers;
    workers.reserve(kWorkers);
    for (int i = 0; i < kWorkers; ++i) {
        workers.push_back(
            pipeline.emplace([i, &workersDone] {
                std::this_thread::sleep_for(kWorkDuration);
                std::printf("  [worker_%d] done\n", i);
                workersDone.fetch_add(1, std::memory_order_relaxed);
            }).name("worker_" + std::to_string(i)).succeed(root)
        );
    }

    // Sink: runs after all workers complete.
    auto sink = pipeline.emplace([&] {
        std::printf("  [sink] all %d workers finished\n", workersDone.load());
    }).name("sink");
    for (auto& w : workers) sink.succeed(w);

    auto exec = makeDesktopExecutor();

    std::printf("\nParallel fan-out/fan-in (%d workers, each %lldms):\n",
                kWorkers, static_cast<long long>(kWorkDuration.count()));

    const auto t0 = std::chrono::steady_clock::now();
    auto result = pipeline.run(*exec);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    if (result) {
        std::printf("\nCompleted in %lld ms  "
                    "(sequential would be ~%lld ms)\n",
                    static_cast<long long>(elapsed.count()),
                    static_cast<long long>(kWorkers * kWorkDuration.count()));
    } else {
        std::printf("Failed.\n");
        return 1;
    }
    return 0;
}
