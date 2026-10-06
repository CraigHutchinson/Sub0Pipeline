// tests/test_observer.cpp
//
// Observer callback tests: job start/finish events, progress monotonicity,
// status values seen for success and failure.

#include <sub0pipeline/sub0pipeline.hpp>
#include "test_helpers.hpp"
#include "doctest.h"

#include <atomic>
#include <latch>
#include <mutex>
#include <ranges>
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
    void onJobStart(RunId, JobId, std::string_view jobName) override
    {
        entries_.push_back({std::string{jobName}, true, {}, 0.0f});
    }

    void onJobFinish(RunId, JobId, std::string_view jobName,
                     JobStatus status, float progress) override
    {
        entries_.push_back({std::string{jobName}, false, status, progress});
    }

    [[nodiscard]] const std::vector<ObserverEntry>& entries() const { return entries_; }

private:
    std::vector<ObserverEntry> entries_;
};

class IdentityObserver final : public IObserver
{
public:
    struct Event {
        RunId runId{};
        JobId jobId{};
        JobStatus status{JobStatus::kPending};
    };
    struct Edge {
        RunId runId{};
        JobId from{};
        JobId to{};
    };

    RunId onRunStart() override { return ++nextRunId_; }

    void onJobStart(RunId runId, JobId jobId, std::string_view) override
    {
        starts_.push_back({runId, jobId, JobStatus::kRunning});
    }

    void onJobFinish(RunId runId, JobId jobId, std::string_view,
                     JobStatus status, float) override
    {
        finishes_.push_back({runId, jobId, status});
    }

    void onDependenciesResolved(RunId runId, JobId from, std::string_view,
                                DependencyRange successors) override
    {
        ++dependencyBatches_;
        for (const auto target : successors)
            edges_.push_back({runId, from, target.id});
    }

    [[nodiscard]] const std::vector<Event>& starts() const { return starts_; }
    [[nodiscard]] const std::vector<Event>& finishes() const { return finishes_; }
    [[nodiscard]] const std::vector<Edge>& edges() const { return edges_; }
    [[nodiscard]] int dependencyBatches() const noexcept { return dependencyBatches_; }

private:
    RunId nextRunId_{40};
    int dependencyBatches_{};
    std::vector<Event> starts_;
    std::vector<Event> finishes_;
    std::vector<Edge> edges_;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Observer tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Observer: successor iterators outlive temporary and copied range wrappers")
{
    static_assert(std::input_iterator<DependencyRange::Iterator>);
    Pipeline pipeline;
    auto root = pipeline.emplace([] {});
    // Exercise arena-backed edges as well as inline successor storage.
    for (int index = 0; index < 6; ++index)
        pipeline.emplace([] {}).name("child").succeed(root);
    auto inlineRoot = pipeline.emplace([] {});
    pipeline.emplace([] {}).name("inline-child").succeed(inlineRoot);

    auto iterator = pipeline.successors(0U).begin();
    auto end = pipeline.successors(0U).end();
    for (JobId id = 1; id <= 6; ++id, ++iterator) {
        REQUIRE(iterator != end);
        CHECK((*iterator).id == id);
        CHECK((*iterator).name == "child");
    }
    CHECK(iterator == end);

    auto range = pipeline.successors(0U);
    auto copy = range;
    CHECK(range.begin() == copy.begin());
    CHECK(range.end() == copy.end());
    CHECK(std::ranges::distance(range) == 6);
    auto inlineIterator = pipeline.successors(7U).begin();
    CHECK((*inlineIterator).id == 8U);
    CHECK((*inlineIterator).name == "inline-child");
    CHECK(pipeline.successors(999U).empty());
    CHECK(pipeline.successors(999U).begin() == pipeline.successors(999U).end());
}

TEST_CASE("Observer: identity-aware events distinguish duplicate names and runs")
{
    Pipeline pipeline;
    InlineExecutor exec;
    IdentityObserver observer;
    auto root = pipeline.emplace([] {}).name("same");
    auto left = pipeline.emplace([] {}).name("same");
    auto right = pipeline.emplace([] {}).name("same");
    left.succeed(root);
    right.succeed(root);

    REQUIRE(pipeline.run(exec, &observer).has_value());
    REQUIRE(pipeline.run(exec, &observer).has_value());

    REQUIRE(observer.starts().size() == 6U);
    CHECK(observer.starts()[0].jobId == 0U);
    CHECK(observer.starts()[1].jobId == 1U);
    CHECK(observer.starts()[2].jobId == 2U);
    CHECK(observer.starts()[0].runId == 41U);
    CHECK(observer.starts()[3].runId == 42U);
    CHECK(observer.finishes()[0].jobId == 0U);
    CHECK(observer.finishes()[1].jobId == 1U);
    CHECK(observer.finishes()[2].jobId == 2U);
    REQUIRE(observer.edges().size() == 4U);
    CHECK(observer.edges()[0].from == 0U);
    CHECK(observer.edges()[0].to == 1U);
    CHECK(observer.edges()[1].from == 0U);
    CHECK(observer.edges()[1].to == 2U);
    CHECK(observer.edges()[0].runId == 41U);
    CHECK(observer.edges()[2].runId == 42U);
    CHECK(observer.dependencyBatches() == 2);
}

TEST_CASE("Observer: parallel dependency events precede successor starts and join before return")
{
    struct ParallelObserver final : IObserver {
        struct Event {
            JobId id{};
            char kind{};
        };
        std::mutex mutex;
        std::vector<Event> events;
        std::atomic<bool> wrongRun{};

        RunId onRunStart() override { return 73U; }
        void record(RunId run, JobId id, char kind)
        {
            if (run != 73U) wrongRun.store(true, std::memory_order_relaxed);
            std::scoped_lock lock{mutex};
            events.push_back({id, kind});
        }
        void onJobStart(RunId run, JobId id, std::string_view) override
        {
            record(run, id, 'S');
        }
        void onJobFinish(RunId run, JobId id, std::string_view, JobStatus, float) override
        {
            record(run, id, 'F');
        }
        void onDependenciesResolved(RunId run, JobId id, std::string_view,
                                    DependencyRange successors) override
        {
            for (const auto target : successors)
                record(run, target.id, 'D');
            record(run, id, 'R');
        }
    } observer;

    Pipeline pipeline;
    std::latch rootsExecuting{2};
    const auto rootBody = [&] {
        rootsExecuting.count_down();
        rootsExecuting.wait();
    };
    auto first = pipeline.emplace(rootBody);
    auto second = pipeline.emplace(rootBody);
    pipeline.emplace([] {}).succeed(first);
    pipeline.emplace([] {}).succeed(second);
    PriorityExecutor executor{{.threadCount = 2}};

    REQUIRE(pipeline.run(executor, &observer).has_value());
    CHECK_FALSE(observer.wrongRun.load(std::memory_order_relaxed));
    REQUIRE(observer.events.size() == 12U);
    for (JobId root = 0; root < 2; ++root) {
        const auto position = [&](JobId id, char kind) {
            return std::ranges::find_if(observer.events, [&](const auto& event) {
                return event.id == id && event.kind == kind;
            });
        };
        const auto finished = position(root, 'F');
        const auto dependency = position(root + 2U, 'D');
        const auto resolved = position(root, 'R');
        const auto started = position(root + 2U, 'S');
        REQUIRE(finished != observer.events.end());
        REQUIRE(dependency != observer.events.end());
        REQUIRE(resolved != observer.events.end());
        REQUIRE(started != observer.events.end());
        CHECK(finished < dependency);
        CHECK(dependency < resolved);
        CHECK(resolved < started);
    }
}

TEST_CASE("Observer: resolved edges include skipped successors")
{
    Pipeline pipeline;
    InlineExecutor exec;
    IdentityObserver observer;
    auto failed = pipeline.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    });
    auto skipped = pipeline.emplace([] {});
    auto downstream = pipeline.emplace([] {});
    skipped.succeed(failed);
    downstream.succeed(skipped);

    CHECK_FALSE(pipeline.run(exec, &observer).has_value());
    REQUIRE(observer.finishes().size() == 3U);
    CHECK(observer.finishes()[1].status == JobStatus::kSkipped);
    CHECK(observer.finishes()[2].status == JobStatus::kSkipped);
    REQUIRE(observer.edges().size() == 2U);
    CHECK(observer.edges()[0].from == 0U);
    CHECK(observer.edges()[0].to == 1U);
    CHECK(observer.edges()[1].from == 1U);
    CHECK(observer.edges()[1].to == 2U);
    CHECK(observer.dependencyBatches() == 2);
}

TEST_CASE("Observer: batch dependency events carry all successor identities")
{
    struct BatchObserver final : IObserver {
        std::vector<JobId> sources;
        std::vector<DependencyRange::Target> targets;

        void onDependenciesResolved(RunId, JobId from, std::string_view,
                                    DependencyRange successors) override
        {
            for (const auto target : successors) {
                sources.push_back(from);
                targets.push_back(target);
            }
        }
    } observer;

    Pipeline pipeline;
    InlineExecutor exec;
    auto root = pipeline.emplace([] {}).name("root");
    auto child = pipeline.emplace([] {}).name("child");
    child.succeed(root);

    REQUIRE(pipeline.run(exec, &observer).has_value());
    REQUIRE(observer.targets.size() == 1U);
    CHECK(observer.sources[0] == 0U);
    CHECK(observer.targets[0].id == 1U);
    CHECK(observer.targets[0].name == "child");
}

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
