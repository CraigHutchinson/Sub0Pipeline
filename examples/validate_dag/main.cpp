// examples/validate_dag/main.cpp
//
// Demonstrates Pipeline::validate(), stream-based dump_text(), and cycle detection.
// Uses SequentialExecutor for deterministic output.
//
// Demo 1 — Valid DAG passes validation:
//   root → (A, B) → sink  (diamond shape, 4 nodes)
//   Explicit validate(), dump_text(std::cout), then run().
//
// Demo 2 — Cycle is caught before execution:
//   X → Y → Z → X   (3-node cycle)
//   validate() returns kCyclicDependency; run() returns the same error.
//
// Demo 3 — Self-loop is caught:
//   A → A   (succeed on itself)
//   validate() returns kCyclicDependency.
//
// Demo 4 — run() calls validate() implicitly:
//   Re-uses the valid diamond DAG from Demo 1.
//   No explicit validate() call — run() handles it automatically.

#include <sub0pipeline/sub0pipeline.hpp>
#include <cstdio>
#include <iostream>

using namespace sub0pipeline;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string_view errorName(PipelineError e) noexcept
{
    switch (e) {
        case PipelineError::kTimeout:          return "kTimeout";
        case PipelineError::kJobFailed:        return "kJobFailed";
        case PipelineError::kCyclicDependency: return "kCyclicDependency";
        case PipelineError::kUnknownJob:       return "kUnknownJob";
    }
    return "unknown";
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main()
{
    SequentialExecutor exec;

    // ── Demo 1: Valid DAG passes validation ───────────────────────────────────
    std::printf("=== Demo 1: Valid DAG passes validation ===\n");
    {
        Pipeline pipeline;

        // Diamond shape: root → (A, B) → sink
        auto root = pipeline.emplace([] { std::printf("  [root] running\n"); }).name("root");
        auto a    = pipeline.emplace([] { std::printf("  [A]    running\n"); }).name("A");
        auto b    = pipeline.emplace([] { std::printf("  [B]    running\n"); }).name("B");
        auto sink = pipeline.emplace([] { std::printf("  [sink] running\n"); }).name("sink");

        a.succeed(root);
        b.succeed(root);
        sink.succeed(a, b);

        // Explicit validation before execution.
        auto valid = pipeline.validate();
        std::printf("  validate() passed: %s\n", valid.has_value() ? "yes" : "no");
        std::printf("  validate()==pass:  %s\n", valid.has_value() ? "pass" : "FAIL");

        // Print the DAG structure for inspection.
        std::printf("  DAG structure (dump_text):\n");
        pipeline.dump_text(std::cout);

        auto result = pipeline.run(exec);
        std::printf("  pipeline.size():   %zu\n", pipeline.size());
        std::printf("  run()==success:    %s\n", result.has_value() ? "pass" : "FAIL");
    }
    std::printf("\n");

    // ── Demo 2: Cycle is caught before execution ──────────────────────────────
    std::printf("=== Demo 2: Cycle caught before execution ===\n");
    {
        Pipeline pipeline;

        auto x = pipeline.emplace([] { std::printf("  [X] running\n"); }).name("X");
        auto y = pipeline.emplace([] { std::printf("  [Y] running\n"); }).name("Y");
        auto z = pipeline.emplace([] { std::printf("  [Z] running\n"); }).name("Z");

        // Form a cycle: X → Y → Z → X
        y.succeed(x);
        z.succeed(y);
        x.succeed(z);  // closes the cycle

        // Explicit validation surfaces the cycle.
        auto valid = pipeline.validate();
        std::printf("  validate() passed: %s\n", valid.has_value() ? "yes" : "no");
        if (!valid) {
            std::printf("  error code:        %s\n", errorName(valid.error()).data());
        }
        std::printf("  error==kCyclic:    %s\n",
            (!valid && valid.error() == PipelineError::kCyclicDependency) ? "pass" : "FAIL");

        // run() also validates internally — it returns the same error.
        auto result = pipeline.run(exec);
        std::printf("  run() also caught: %s\n",
            (!result && result.error() == PipelineError::kCyclicDependency) ? "pass" : "FAIL");
    }
    std::printf("\n");

    // ── Demo 3: Self-loop is caught ───────────────────────────────────────────
    std::printf("=== Demo 3: Self-loop caught ===\n");
    {
        Pipeline pipeline;

        auto a = pipeline.emplace([] { std::printf("  [A] running\n"); }).name("A");

        // A depends on itself — a degenerate cycle of length 1.
        a.succeed(a);

        auto valid = pipeline.validate();
        std::printf("  validate() passed: %s\n", valid.has_value() ? "yes" : "no");
        if (!valid) {
            std::printf("  error code:        %s\n", errorName(valid.error()).data());
        }
        std::printf("  error==kCyclic:    %s\n",
            (!valid && valid.error() == PipelineError::kCyclicDependency) ? "pass" : "FAIL");
    }
    std::printf("\n");

    // ── Demo 4: run() calls validate() implicitly ─────────────────────────────
    std::printf("=== Demo 4: run() validates implicitly ===\n");
    {
        Pipeline pipeline;

        // Same diamond DAG as Demo 1.
        auto root = pipeline.emplace([] { std::printf("  [root] running\n"); }).name("root");
        auto a    = pipeline.emplace([] { std::printf("  [A]    running\n"); }).name("A");
        auto b    = pipeline.emplace([] { std::printf("  [B]    running\n"); }).name("B");
        auto sink = pipeline.emplace([] { std::printf("  [sink] running\n"); }).name("sink");

        a.succeed(root);
        b.succeed(root);
        sink.succeed(a, b);

        // No explicit validate() here — run() performs it automatically before
        // dispatching any jobs. If the DAG were invalid, run() would return
        // kCyclicDependency without executing anything.
        auto result = pipeline.run(exec);

        std::printf("  run()==success:    %s\n", result.has_value() ? "pass" : "FAIL");
        std::printf("  pipeline.size():   %zu\n", pipeline.size());
    }
    std::printf("\n");

    return 0;
}
