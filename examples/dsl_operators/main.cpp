// examples/dsl_operators/main.cpp
//
// Demonstrates the Sub0Pipeline DSL extension: operator-based dependency
// wiring, the _job UDL for named job creation, and inline pipe syntax.
//
// Graph built:  load → (parse ∥ validate) → commit
//
// Compare with examples/boot_sequence/main.cpp for the same pattern
// expressed with the core API.

#include <sub0pipeline/dsl.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

namespace sub0pipeline { std::unique_ptr<IExecutor> makeDesktopExecutor(); }

using namespace std::chrono_literals;
using namespace sub0pipeline;
using namespace sub0pipeline::dsl;

namespace {

auto load_data() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(20ms);
    std::printf("  [load]     done\n");
    return {};
}

auto parse_input() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(80ms);
    std::printf("  [parse]    done\n");
    return {};
}

auto validate_input() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(120ms);
    std::printf("  [validate] done\n");
    return {};
}

auto commit_result() -> std::expected<void, PipelineError>
{
    std::printf("  [commit]   done\n");
    return {};
}

} // namespace

int main()
{
    // ── Demo 1: Structured bindings + operator wiring ─────────────────────
    {
        std::printf("=== Demo 1: Structured bindings + operator wiring ===\n");
        Pipeline pipe;
        auto [load, parse, validate, commit] = pipe.emplace(
            "load"_job(load_data),
            "parse"_job(parse_input).timeout(500ms),
            "validate"_job(validate_input).timeout(500ms),
            "commit"_job(commit_result)
        );

        load >> parse + validate >> commit;

        auto exec = makeDesktopExecutor();
        auto result = pipe.run(*exec);
        std::printf("  Result: %s\n\n", result ? "OK" : "FAILED");
    }

    // ── Demo 2: Full inline pipe — entire DAG in ONE expression ───────────
    {
        std::printf("=== Demo 2: Inline pipe syntax ===\n");
        Pipeline pipe;
        pipe >> "load"_job(load_data)
             >> "parse"_job(parse_input).timeout(500ms) + "validate"_job(validate_input).timeout(500ms)
             >> "commit"_job(commit_result);

        auto exec = makeDesktopExecutor();
        auto result = pipe.run(*exec);
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
