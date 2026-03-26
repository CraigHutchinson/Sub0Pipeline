// examples/boot_sequence/main.cpp
//
// Simulates a device boot sequence where:
//   nvs → (display ∥ network) → app
//
// 'display' and 'network' have no dependency on each other, so the
// DesktopExecutor runs them in parallel via std::thread.

#include <sub0pipeline/sub0pipeline.hpp>
#include <chrono>
#include <cstdio>
#include <thread>

namespace sub0pipeline { std::unique_ptr<IExecutor> make_desktop_executor(); }

using namespace std::chrono_literals;

namespace {

// Simulated subsystem initialisers (sleep to mimic real work).
auto nvs_init() -> std::expected<void, sub0pipeline::PipelineError>
{
    std::this_thread::sleep_for(20ms);
    std::printf("  [nvs]     initialised\n");
    return {};
}

auto display_init() -> std::expected<void, sub0pipeline::PipelineError>
{
    std::this_thread::sleep_for(80ms);
    std::printf("  [display] initialised\n");
    return {};
}

auto network_init() -> std::expected<void, sub0pipeline::PipelineError>
{
    std::this_thread::sleep_for(120ms);
    std::printf("  [network] initialised\n");
    return {};
}

auto app_start() -> std::expected<void, sub0pipeline::PipelineError>
{
    std::printf("  [app]     started\n");
    return {};
}

} // namespace

int main()
{
    using namespace sub0pipeline;

    Pipeline boot;

    auto nvs     = boot.emplace(nvs_init).name("nvs");
    auto display = boot.emplace(display_init).name("display").timeout(500ms);
    auto network = boot.emplace(network_init).name("network").timeout(500ms);
    auto app     = boot.emplace(app_start).name("app");

    // display and network both depend on nvs but NOT on each other.
    display.succeed(nvs);
    network.succeed(nvs);
    app.succeed(display, network);

    auto exec = make_desktop_executor();

    std::printf("Boot sequence starting...\n");
    const auto t0 = std::chrono::steady_clock::now();

    auto result = boot.run(*exec);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    if (result) {
        std::printf("Boot complete in %lld ms  "
                    "(sequential would be ~%d ms)\n",
                    static_cast<long long>(elapsed.count()),
                    20 + 80 + 120);
    } else {
        std::printf("Boot failed.\n");
        return 1;
    }
    return 0;
}
