// examples/error_handling/main.cpp
//
// Demonstrates all Pipeline failure modes using a sequential executor for
// deterministic, reproducible output.
//
// Demo 1 — Required job failure propagates:
//   A → B (fails) → C          run() returns kJobFailed; C is skipped.
//
// Demo 2 — Optional job failure is bypassed:
//   A → B (fails, optional) → C   run() succeeds; C still runs.
//
// Demo 3 — Multiple independent failures, first error captured:
//   X (fails kJobFailed) and Y (fails kTimeout) run as independent roots.
//   run() returns an error; both end up kFailed.
//
// Demo 4 — expected<>-returning jobs:
//   Emplace lambdas that return std::expected<void, PipelineError> directly.
//   One succeeds, one fails; pipeline returns the failure.

#include <sub0pipeline/sub0pipeline.hpp>
#include <cstdio>
#include <expected>

// Forward-declared in sequential_executor.cpp (Sub0Pipeline::Headless).
namespace sub0pipeline { std::unique_ptr<IExecutor> makeSequentialExecutor(); }

using namespace sub0pipeline;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string_view statusName(JobStatus s) noexcept
{
    switch (s) {
        case JobStatus::kPending:  return "kPending";
        case JobStatus::kReady:    return "kReady";
        case JobStatus::kRunning:  return "kRunning";
        case JobStatus::kDone:     return "kDone";
        case JobStatus::kFailed:   return "kFailed";
        case JobStatus::kSkipped:  return "kSkipped";
        case JobStatus::kTimedOut: return "kTimedOut";
    }
    return "unknown";
}

static std::string_view errorName(PipelineError e) noexcept
{
    switch (e) {
        case PipelineError::kTimeout:          return "kTimeout";
        case PipelineError::kJobFailed:        return "kJobFailed";
        case PipelineError::kDependencyFailed: return "kDependencyFailed";
        case PipelineError::kCyclicDependency: return "kCyclicDependency";
        case PipelineError::kDuplicateJob:     return "kDuplicateJob";
        case PipelineError::kUnknownJob:       return "kUnknownJob";
    }
    return "unknown";
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main()
{
    auto exec = makeSequentialExecutor();

    // ── Demo 1: Required job failure propagates ───────────────────────────────
    std::printf("=== Demo 1: Required job failure propagates ===\n");
    {
        Pipeline pipeline;

        auto a = pipeline.emplace([] { std::printf("  [A] running\n"); }).name("A");
        auto b = pipeline.emplace(
            []() -> std::expected<void, PipelineError> {
                std::printf("  [B] failing\n");
                return std::unexpected(PipelineError::kJobFailed);
            }
        ).name("B");
        auto c = pipeline.emplace([] { std::printf("  [C] running\n"); }).name("C");

        b.succeed(a);
        c.succeed(b);

        auto result = pipeline.run(*exec);

        std::printf("  run() succeeded:  %s\n", result.has_value() ? "yes" : "no");
        if (!result) {
            std::printf("  error code:       %s\n", errorName(result.error()).data());
        }
        std::printf("  status(A):        %s\n", statusName(pipeline.status(a)).data());
        std::printf("  status(B):        %s\n", statusName(pipeline.status(b)).data());
        std::printf("  status(C):        %s\n", statusName(pipeline.status(c)).data());

        // Verify expected invariants.
        std::printf("  A==kDone:         %s\n",
            pipeline.status(a) == JobStatus::kDone    ? "pass" : "FAIL");
        std::printf("  B==kFailed:       %s\n",
            pipeline.status(b) == JobStatus::kFailed  ? "pass" : "FAIL");
        std::printf("  C==kSkipped:      %s\n",
            pipeline.status(c) == JobStatus::kSkipped ? "pass" : "FAIL");
        std::printf("  error==kJobFailed:%s\n",
            (!result && result.error() == PipelineError::kJobFailed) ? "pass" : "FAIL");
    }
    std::printf("\n");

    // ── Demo 2: Optional job failure is bypassed ──────────────────────────────
    std::printf("=== Demo 2: Optional job failure bypassed ===\n");
    {
        Pipeline pipeline;
        bool cRan = false;

        auto a = pipeline.emplace([] { std::printf("  [A] running\n"); }).name("A");
        auto b = pipeline.emplace(
            []() -> std::expected<void, PipelineError> {
                std::printf("  [B] failing (optional)\n");
                return std::unexpected(PipelineError::kJobFailed);
            }
        ).name("B").optional();
        auto c = pipeline.emplace([&cRan] {
            std::printf("  [C] running\n");
            cRan = true;
        }).name("C");

        b.succeed(a);
        c.succeed(b);

        auto result = pipeline.run(*exec);

        std::printf("  run() succeeded:  %s\n", result.has_value() ? "yes" : "no");
        std::printf("  C ran:            %s\n", cRan ? "yes" : "no");
        std::printf("  status(B):        %s\n", statusName(pipeline.status(b)).data());
        std::printf("  status(C):        %s\n", statusName(pipeline.status(c)).data());

        std::printf("  run()==success:   %s\n", result.has_value()                      ? "pass" : "FAIL");
        std::printf("  C ran:            %s\n", cRan                                    ? "pass" : "FAIL");
        std::printf("  B==kFailed:       %s\n", pipeline.status(b) == JobStatus::kFailed ? "pass" : "FAIL");
        std::printf("  C==kDone:         %s\n", pipeline.status(c) == JobStatus::kDone   ? "pass" : "FAIL");
    }
    std::printf("\n");

    // ── Demo 3: Multiple independent failures, first error captured ───────────
    std::printf("=== Demo 3: Multiple independent failures ===\n");
    {
        Pipeline pipeline;

        // X and Y are independent roots — no succeed/precede between them.
        auto x = pipeline.emplace(
            []() -> std::expected<void, PipelineError> {
                std::printf("  [X] failing with kJobFailed\n");
                return std::unexpected(PipelineError::kJobFailed);
            }
        ).name("X");

        auto y = pipeline.emplace(
            []() -> std::expected<void, PipelineError> {
                std::printf("  [Y] failing with kTimeout\n");
                return std::unexpected(PipelineError::kTimeout);
            }
        ).name("Y");

        auto result = pipeline.run(*exec);

        std::printf("  run() succeeded:  %s\n", result.has_value() ? "yes" : "no");
        if (!result) {
            std::printf("  error code:       %s\n", errorName(result.error()).data());
        }
        std::printf("  status(X):        %s\n", statusName(pipeline.status(x)).data());
        std::printf("  status(Y):        %s\n", statusName(pipeline.status(y)).data());

        std::printf("  run()==error:     %s\n", !result                                  ? "pass" : "FAIL");
        std::printf("  X==kFailed:       %s\n", pipeline.status(x) == JobStatus::kFailed  ? "pass" : "FAIL");
        std::printf("  Y==kFailed:       %s\n", pipeline.status(y) == JobStatus::kFailed  ? "pass" : "FAIL");
    }
    std::printf("\n");

    // ── Demo 4: expected<>-returning jobs ─────────────────────────────────────
    std::printf("=== Demo 4: expected<>-returning jobs ===\n");
    {
        Pipeline pipeline;

        // A job that explicitly returns success via std::expected<void, PipelineError>.
        auto good = pipeline.emplace(
            []() -> std::expected<void, PipelineError> {
                std::printf("  [good] returning success\n");
                return {};  // value-initialised expected == success
            }
        ).name("good");

        // A job that explicitly returns failure via std::unexpected.
        auto bad = pipeline.emplace(
            []() -> std::expected<void, PipelineError> {
                std::printf("  [bad] returning kJobFailed\n");
                return std::unexpected(PipelineError::kJobFailed);
            }
        ).name("bad");

        // bad does not depend on good — both are independent roots.
        (void)good;

        auto result = pipeline.run(*exec);

        std::printf("  run() succeeded:  %s\n", result.has_value() ? "yes" : "no");
        if (!result) {
            std::printf("  error code:       %s\n", errorName(result.error()).data());
        }
        std::printf("  status(good):     %s\n", statusName(pipeline.status(good)).data());
        std::printf("  status(bad):      %s\n", statusName(pipeline.status(bad)).data());

        std::printf("  good==kDone:      %s\n", pipeline.status(good) == JobStatus::kDone   ? "pass" : "FAIL");
        std::printf("  bad==kFailed:     %s\n", pipeline.status(bad)  == JobStatus::kFailed  ? "pass" : "FAIL");
        std::printf("  error==kJobFailed:%s\n",
            (!result && result.error() == PipelineError::kJobFailed) ? "pass" : "FAIL");
    }
    std::printf("\n");

    return 0;
}
