// examples/dsl_operators/main.cpp
//
// Demonstrates the Sub0Pipeline DSL extension: operator-based dependency
// wiring, the _job UDL for named job creation, and inline pipe syntax.
//
// Graph built:  nvs → (display ∥ network) → app
//
// Compare with examples/boot_sequence/main.cpp — same pipeline, expressed
// three different ways.

#define SUB0PIPELINE_ENABLE_DSL 1
#include <sub0pipeline/dsl.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

namespace sub0pipeline { std::unique_ptr<IExecutor> makeDesktopExecutor(); }

using namespace std::chrono_literals;
using namespace sub0pipeline;
using namespace sub0pipeline::dsl;

namespace {

auto nvs_init() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(20ms);
    std::printf("  [nvs]     initialised\n");
    return {};
}

auto display_init() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(80ms);
    std::printf("  [display] initialised\n");
    return {};
}

auto network_init() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(120ms);
    std::printf("  [network] initialised\n");
    return {};
}

auto app_start() -> std::expected<void, PipelineError>
{
    std::printf("  [app]     started\n");
    return {};
}

} // namespace

int main()
{
    // ── Demo 1: Structured bindings + operator wiring ─────────────────────
    {
        std::printf("=== Demo 1: Structured bindings + operator wiring ===\n");
        Pipeline boot;
        auto [nvs, display, network, app] = boot.emplace(
            "nvs"_job(nvs_init),
            "display"_job(display_init).timeout(500ms),
            "network"_job(network_init).timeout(500ms),
            "app"_job(app_start)
        );

        nvs >> display + network >> app;

        auto exec = makeDesktopExecutor();
        auto result = boot.run(*exec);
        std::printf("  Result: %s\n\n", result ? "OK" : "FAILED");
    }

    // ── Demo 2: Full inline pipe — entire boot in ONE expression ──────────
    {
        std::printf("=== Demo 2: Inline pipe syntax ===\n");
        Pipeline boot;
        boot >> "nvs"_job(nvs_init)
             >> "display"_job(display_init).timeout(500ms) + "network"_job(network_init).timeout(500ms)
             >> "app"_job(app_start);

        auto exec = makeDesktopExecutor();
        auto result = boot.run(*exec);
        std::printf("  Result: %s\n\n", result ? "OK" : "FAILED");
    }

    // ── Demo 3: Linear chain — unnamed fire-and-forget jobs ───────────────
    {
        std::printf("=== Demo 3: Unnamed linear chain ===\n");
        Pipeline pipe;
        pipe >> job([] { std::printf("  step 1\n"); })
             >> job([] { std::printf("  step 2\n"); })
             >> job([] { std::printf("  step 3\n"); });

        auto exec = makeDesktopExecutor();
        auto result = pipe.run(*exec);
        std::printf("  Result: %s\n\n", result ? "OK" : "FAILED");
    }

    // ── Demo 4: Pre-emplaced root + sink with inline middle ───────────────
    {
        std::printf("=== Demo 4: Emplaced root/sink + inline middle ===\n");
        Pipeline pipe;
        auto root = pipe.emplace("setup"_job([] { std::printf("  [setup]   done\n"); }));
        auto sink = pipe.emplace("finish"_job([] { std::printf("  [finish]  done\n"); }));

        root >> "work_a"_job([] { std::printf("  [work_a]  done\n"); })
              + "work_b"_job([] { std::printf("  [work_b]  done\n"); })
             >> sink;

        auto exec = makeDesktopExecutor();
        auto result = pipe.run(*exec);
        std::printf("  Result: %s\n\n", result ? "OK" : "FAILED");
    }

    return 0;
}
