// examples/on_demand_jobs/main.cpp
//
// Demonstrates add_on_demand() — registering a job that can be triggered
// by an external event (e.g. button press, ISR, network message) after the
// boot pipeline has completed.
//
// Note: trigger() is currently a stub; this example shows the API contract.

#include <sub0pipeline/sub0pipeline.hpp>
#include <cstdio>

namespace sub0pipeline { std::unique_ptr<IExecutor> makeSequentialExecutor(); }

using namespace sub0pipeline;

int main()
{
    auto exec = makeSequentialExecutor();

    // ── Boot pipeline ─────────────────────────────────────────────────────────
    Pipeline boot;
    boot.emplace([] { std::printf("  [boot] init complete\n"); }).name("init");
    (void)boot.run(*exec);

    // ── Register on-demand jobs ───────────────────────────────────────────────
    Pipeline events;

    auto reboot = events.add_on_demand([]() -> std::expected<void, PipelineError>
    {
        std::printf("  [event] reboot requested\n");
        return {};
    });
    reboot.name("reboot");

    auto ota = events.add_on_demand([]() -> std::expected<void, PipelineError>
    {
        std::printf("  [event] OTA update triggered\n");
        return {};
    });
    ota.name("ota_update");

    // ── Simulate triggering events ────────────────────────────────────────────
    std::printf("\nSimulating event triggers:\n");

    // trigger() is a no-op stub currently — demonstrates the intended API.
    events.trigger(reboot);
    events.trigger(ota);

    std::printf("  (trigger() is a stub — see PLATFORM_ROADMAP.md)\n");

    return 0;
}
