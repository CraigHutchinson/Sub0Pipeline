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

namespace sub0pipeline { std::unique_ptr<IExecutor> make_desktop_executor(); }

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
    for (int i = 0; i < kWorkers; ++i) {
        pipeline.emplace([i, &workersDone]
        {
            std::this_thread::sleep_for(kWorkDuration);
            std::printf("  [worker_%d] done\n", i);
            workersDone.fetch_add(1, std::memory_order_relaxed);
        }).name("worker_" + std::to_string(i)).succeed(root);
    }

    // Sink: runs after all workers complete.
    pipeline.emplace([&]
    {
        std::printf("  [sink] all %d workers finished\n", workersDone.load());
    }).name("sink");

    // Wire sink to depend on all workers.
    // (Each worker already precedes sink via the DAG structure built above.)
    // The last emplace returned the sink job — rewire it:
    {
        // Rebuild with explicit sink handle.
        Pipeline p2;
        auto r2 = p2.emplace([] { std::printf("  [root] started\n"); }).name("root");
        std::vector<Job> workers;
        workers.reserve(kWorkers);
        for (int i = 0; i < kWorkers; ++i) {
            workers.push_back(
                p2.emplace([i, &workersDone] {
                    std::this_thread::sleep_for(kWorkDuration);
                    std::printf("  [worker_%d] done\n", i);
                    workersDone.fetch_add(1, std::memory_order_relaxed);
                }).name("worker_" + std::to_string(i)).succeed(r2)
            );
        }
        auto sink2 = p2.emplace([&] {
            std::printf("  [sink] all %d workers finished\n", workersDone.load());
        }).name("sink");
        for (auto& w : workers) sink2.succeed(w);

        auto exec = make_desktop_executor();

        std::printf("\nParallel fan-out/fan-in (%d workers, each %lldms):\n",
                    kWorkers, static_cast<long long>(kWorkDuration.count()));

        const auto t0 = std::chrono::steady_clock::now();
        auto result = p2.run(*exec);
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
    }
    return 0;
}
