// examples/on_demand_jobs/main.cpp
//
// Demonstrates add_on_demand() + arm() + trigger():
// - On-demand jobs are excluded from the normal run() execution phase.
// - arm() stores an executor for later trigger() calls.
// - trigger() dispatches the job immediately via the armed executor.
//
// Typical use case: a boot pipeline runs to completion, then on-demand
// jobs handle asynchronous events (network message, ISR, queue item).

#include <sub0pipeline/sub0pipeline.hpp>
#include <cstdio>
#include <atomic>

namespace sub0pipeline {
    std::unique_ptr<IExecutor> makeDesktopExecutor();
    std::unique_ptr<IExecutor> makeSequentialExecutor();
}

using namespace sub0pipeline;

int main()
{
    auto exec = makeDesktopExecutor();

    // ── Boot pipeline -- runs to completion ───────────────────────────────────
    {
        Pipeline boot;
        boot.emplace([] { std::printf("  [boot] init complete\n"); }).name("init");
        (void)boot.run(*exec);
    }
    std::printf("\n");

    // ── On-demand event pipeline ──────────────────────────────────────────────
    Pipeline events;

    std::atomic<int> rebootCount{0};
    std::atomic<int> otaCount{0};

    auto reboot = events.add_on_demand([&]() -> std::expected<void, PipelineError>
    {
        std::printf("  [event] reboot requested (count=%d)\n",
                    rebootCount.fetch_add(1, std::memory_order_relaxed) + 1);
        return {};
    });
    reboot.name("reboot");

    auto ota = events.add_on_demand([&]() -> std::expected<void, PipelineError>
    {
        std::printf("  [event] OTA update triggered (count=%d)\n",
                    otaCount.fetch_add(1, std::memory_order_relaxed) + 1);
        return {};
    });
    ota.name("ota_update");

    // arm() stores the executor for trigger() calls.
    events.arm(*exec);

    // Verify on-demand jobs do NOT run during normal run() (no normal jobs here,
    // so the pipeline is empty from run()'s perspective).
    (void)events.run(*exec);
    exec->wait_all();
    std::printf("After run(): rebootCount=%d otaCount=%d (both should be 0)\n\n",
                rebootCount.load(), otaCount.load());

    // ── Simulate triggering events ────────────────────────────────────────────
    std::printf("Simulating event triggers:\n");

    if (auto r = events.trigger(reboot); !r)
        std::printf("  trigger(reboot) failed\n");

    if (auto r = events.trigger(ota); !r)
        std::printf("  trigger(ota) failed\n");

    if (auto r = events.trigger(reboot); !r)
        std::printf("  trigger(reboot) #2 failed\n");

    exec->wait_all();

    std::printf("\nFinal counts: reboot=%d ota=%d\n",
                rebootCount.load(), otaCount.load());
    // Expected: reboot=2, ota=1

    return 0;
}
