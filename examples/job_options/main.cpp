// examples/job_options/main.cpp
//
// Reference example: every Job builder method and the precede() direction.
// Uses makeSequentialExecutor() for deterministic, reproducible output.
//
// Demo 1 — All builder methods:
//   Shows .name(), .timeout(), .optional(), .status(), .core(),
//   .priority(), .stack() on a 3-job pipeline.
//   NOTE: .core(), .priority(), .stack() are hints consumed by
//   FreeRtosExecutor (PriorityExecutor honours .priority() only); the
//   Sequential and Desktop executors ignore them.
//
// Demo 2 — precede() vs succeed():
//   a.precede(b) and b.succeed(a) express the same dependency from
//   two authoring perspectives.  Both pipelines run in the same order.
//
// Demo 3 — optional() chains:
//   required_A → optional_B(fails) → required_C   C still runs.
//   required_A → required_D(fails) → required_E   E is skipped.
//
// Demo 4 — Job validity and comparison:
//   Job::valid(), operator bool(), operator==.

#include <sub0pipeline/sub0pipeline.hpp>
#include <cstdio>
#include <expected>

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

// ── Main ──────────────────────────────────────────────────────────────────────

int main()
{
    auto exec = makeSequentialExecutor();

    // ── Demo 1: All builder methods ───────────────────────────────────────────
    std::printf("=== Demo 1: All builder methods ===\n");
    {
        Pipeline pipeline;

        // All builder methods demonstrated on a 3-job pipeline.
        // .core(), .priority(), .stack() are FreeRtosExecutor hints;
        // SequentialExecutor ignores them but the API is fully accepted.
        auto a = pipeline.emplace([] {
            std::printf("  [A] running\n");
        }).name("loader")
          .timeout(std::chrono::milliseconds{500})
          .status("Loading…")
          .core(0)
          .priority(10)
          .stack(8192);

        auto b = pipeline.emplace([] {
            std::printf("  [B] running\n");
        }).name("configure")
          .timeout(std::chrono::milliseconds{250})
          .optional(false)   // explicitly required (the default)
          .status("Configuring…")
          .core(-1)          // any core
          .priority(5)
          .stack(4096);

        auto c = pipeline.emplace([] {
            std::printf("  [C] running\n");
        }).name("start")
          .optional();       // this one may fail without blocking C's successor

        b.succeed(a);
        c.succeed(b);

        auto result = pipeline.run(*exec);

        std::printf("  pipeline.size():  %zu\n", pipeline.size());
        std::printf("  name(a):          %.*s\n",
                    static_cast<int>(pipeline.name(a).size()),
                    pipeline.name(a).data());
        std::printf("  run()==success:   %s\n", result.has_value() ? "pass" : "FAIL");
    }
    std::printf("\n");

    // ── Demo 2: precede() vs succeed() ───────────────────────────────────────
    std::printf("=== Demo 2: precede() vs succeed() ===\n");
    {
        // These two pipelines express identical dependencies:
        //   Pipeline A: b.succeed(a)  — "b runs after a"
        //   Pipeline B: a.precede(b)  — "a runs before b"

        bool aFirst = false;
        bool bFirst = false;

        std::printf("  -- Using succeed() --\n");
        {
            Pipeline pipeline;
            bool ranA = false;

            auto a = pipeline.emplace([&ranA] {
                ranA = true;
                std::printf("    [A] running\n");
            }).name("A");

            auto b = pipeline.emplace([&ranA, &aFirst] {
                aFirst = ranA;   // true means A completed before B started
                std::printf("    [B] running (A ran first: %s)\n", ranA ? "yes" : "no");
            }).name("B");

            b.succeed(a);    // B runs after A
            if (!pipeline.run(*exec)) { std::printf("    run() failed\n"); }
        }

        std::printf("  -- Using precede() --\n");
        {
            Pipeline pipeline;
            bool ranA = false;

            auto a = pipeline.emplace([&ranA] {
                ranA = true;
                std::printf("    [A] running\n");
            }).name("A");

            auto b = pipeline.emplace([&ranA, &bFirst] {
                bFirst = ranA;   // true means A completed before B started
                std::printf("    [B] running (A ran first: %s)\n", ranA ? "yes" : "no");
            }).name("B");

            a.precede(b);    // A runs before B — same dependency, opposite phrasing
            if (!pipeline.run(*exec)) { std::printf("    run() failed\n"); }
        }

        std::printf("  succeed(): A before B: %s\n", aFirst ? "pass" : "FAIL");
        std::printf("  precede(): A before B: %s\n", bFirst ? "pass" : "FAIL");
        std::printf("  both orderings equal:  %s\n", (aFirst == bFirst) ? "pass" : "FAIL");
    }
    std::printf("\n");

    // ── Demo 3: optional() chains ─────────────────────────────────────────────
    std::printf("=== Demo 3: optional() chains ===\n");

    std::printf("  -- Chain 1: required_A → optional_B(fails) → required_C --\n");
    {
        Pipeline pipeline;
        bool cRan = false;

        auto a = pipeline.emplace([] {
            std::printf("    [A] running\n");
        }).name("required_A");

        auto b = pipeline.emplace(
            []() -> std::expected<void, PipelineError> {
                std::printf("    [B] failing (optional)\n");
                return std::unexpected(PipelineError::kJobFailed);
            }
        ).name("optional_B").optional();

        auto c = pipeline.emplace([&cRan] {
            std::printf("    [C] running\n");
            cRan = true;
        }).name("required_C");

        b.succeed(a);
        c.succeed(b);

        auto result = pipeline.run(*exec);

        std::printf("  run()==success:   %s\n", result.has_value() ? "pass" : "FAIL");
        std::printf("  C ran:            %s\n", cRan                                     ? "pass" : "FAIL");
        std::printf("  B==kFailed:       %s\n", pipeline.status(b) == JobStatus::kFailed  ? "pass" : "FAIL");
        std::printf("  C==kDone:         %s\n", pipeline.status(c) == JobStatus::kDone    ? "pass" : "FAIL");
    }
    std::printf("\n");

    std::printf("  -- Chain 2: required_A → required_D(fails) → required_E --\n");
    {
        Pipeline pipeline;
        bool eRan = false;

        auto a = pipeline.emplace([] {
            std::printf("    [A] running\n");
        }).name("required_A");

        auto d = pipeline.emplace(
            []() -> std::expected<void, PipelineError> {
                std::printf("    [D] failing (required)\n");
                return std::unexpected(PipelineError::kJobFailed);
            }
        ).name("required_D");   // NOT optional — default

        auto e = pipeline.emplace([&eRan] {
            std::printf("    [E] running\n");
            eRan = true;
        }).name("required_E");

        d.succeed(a);
        e.succeed(d);

        auto result = pipeline.run(*exec);

        std::printf("  run()==error:     %s\n", !result                                   ? "pass" : "FAIL");
        std::printf("  E ran:            %s\n", !eRan                                     ? "pass" : "FAIL");
        std::printf("  D==kFailed:       %s\n", pipeline.status(d) == JobStatus::kFailed  ? "pass" : "FAIL");
        std::printf("  E==kSkipped:      %s\n", pipeline.status(e) == JobStatus::kSkipped ? "pass" : "FAIL");
    }
    std::printf("\n");

    // ── Demo 4: Job validity and comparison ───────────────────────────────────
    std::printf("=== Demo 4: Job validity and comparison ===\n");
    {
        Pipeline pipeline;

        Job invalid;                            // default-constructed: no node
        auto j = pipeline.emplace([] {});       // valid handle
        auto k = pipeline.emplace([] {});       // different node

        // valid() / operator bool
        std::printf("  invalid.valid():  %s\n", !invalid.valid()  ? "pass" : "FAIL");
        std::printf("  j.valid():        %s\n",  j.valid()        ? "pass" : "FAIL");
        std::printf("  bool(j):          %s\n",  static_cast<bool>(j) ? "pass" : "FAIL");
        std::printf("  bool(invalid):    %s\n", !static_cast<bool>(invalid) ? "pass" : "FAIL");

        // operator==
        std::printf("  j == j:           %s\n", (j == j)         ? "pass" : "FAIL");
        std::printf("  j != k:           %s\n", !(j == k)        ? "pass" : "FAIL");
        std::printf("  invalid == invalid:%s\n", (invalid == invalid) ? "pass" : "FAIL");
        std::printf("  j != invalid:     %s\n", !(j == invalid)  ? "pass" : "FAIL");
    }
    std::printf("\n");

    return 0;
}
