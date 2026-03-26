// tests/test_observer.cpp
//
// Observer callback tests: on_start / on_finish firing, progress monotonicity,
// status values seen for success and failure.

#include <sub0pipeline/sub0pipeline.hpp>
#include "test_helpers.hpp"
#include "doctest.h"

#include <string>
#include <vector>

using namespace sub0pipeline;

// ── Helpers ───────────────────────────────────────────────────────────────────

struct ObserverEntry
{
    std::string name;
    bool        isStart{};
    JobStatus   status{};
    float       progress{};
};

class RecordingObserver final : public IObserver
{
public:
    void onStart(std::string_view jobName) override
    {
        entries_.push_back({std::string{jobName}, true, {}, 0.0f});
    }

    void onFinish(std::string_view jobName, JobStatus status, float progress) override
    {
        entries_.push_back({std::string{jobName}, false, status, progress});
    }

    [[nodiscard]] const std::vector<ObserverEntry>& entries() const { return entries_; }

private:
    std::vector<ObserverEntry> entries_;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Observer tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Observer: on_start and on_finish fired for single job")
{
    Pipeline          pipeline;
    InlineExecutor    exec;
    RecordingObserver obs;

    pipeline.emplace([] {}).name("job1");
    (void)pipeline.run(exec, &obs);

    const auto& entries = obs.entries();
    REQUIRE(entries.size() >= 2U);
    CHECK(entries[0].name    == "job1");
    CHECK(entries[0].isStart == true);
    CHECK(entries[1].name    == "job1");
    CHECK(entries[1].isStart == false);
    CHECK(entries[1].status  == JobStatus::kDone);
}

TEST_CASE("Observer: progress increases monotonically")
{
    Pipeline          pipeline;
    InlineExecutor    exec;
    RecordingObserver obs;

    pipeline.emplace([] {}).name("A");
    pipeline.emplace([] {}).name("B");
    pipeline.emplace([] {}).name("C");

    (void)pipeline.run(exec, &obs);

    float lastProgress = -1.0f;
    for (const auto& e : obs.entries()) {
        if (!e.isStart) {
            CHECK(e.progress > lastProgress);
            lastProgress = e.progress;
        }
    }
    // Final progress must be ~1.0
    CHECK(lastProgress == doctest::Approx(1.0f).epsilon(0.01f));
}

TEST_CASE("Observer: kFailed status reported for failing job")
{
    Pipeline          pipeline;
    InlineExecutor    exec;
    RecordingObserver obs;

    pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("fail_job").optional();  // optional so run() succeeds

    (void)pipeline.run(exec, &obs);

    bool sawFailure = false;
    for (const auto& e : obs.entries()) {
        if (!e.isStart && e.status == JobStatus::kFailed) sawFailure = true;
    }
    CHECK(sawFailure);
}

TEST_CASE("Observer: null observer is safe (no crash)")
{
    Pipeline       pipeline;
    InlineExecutor exec;

    pipeline.emplace([] {}).name("A");
    auto result = pipeline.run(exec, nullptr);
    CHECK(result.has_value());
}

TEST_CASE("Observer: start fired before finish for each job")
{
    Pipeline          pipeline;
    InlineExecutor    exec;
    RecordingObserver obs;

    pipeline.emplace([] {}).name("X");
    pipeline.emplace([] {}).name("Y");
    (void)pipeline.run(exec, &obs);

    // Verify each finish entry is preceded by its matching start entry.
    for (std::size_t i = 0U; i < obs.entries().size(); ++i) {
        const auto& e = obs.entries()[i];
        if (!e.isStart) {
            // Find the matching start
            bool foundStart = false;
            for (std::size_t j = 0U; j < i; ++j) {
                if (obs.entries()[j].isStart && obs.entries()[j].name == e.name) {
                    foundStart = true;
                    break;
                }
            }
            CHECK(foundStart);
        }
    }
}

TEST_CASE("Observer: kSkipped status reported for downstream of required failure")
{
    Pipeline       pipeline;
    InlineExecutor exec;
    RecordingObserver obs;

    auto req = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    }).name("required");

    auto dep = pipeline.emplace([] {}).name("dependent");
    dep.succeed(req);

    (void)pipeline.run(exec, &obs);

    bool sawSkipped = false;
    for (const auto& e : obs.entries()) {
        if (!e.isStart && e.name == "dependent" && e.status == JobStatus::kSkipped)
            sawSkipped = true;
    }
    CHECK(sawSkipped);
}
