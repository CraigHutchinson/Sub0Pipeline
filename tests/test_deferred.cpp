// tests/test_deferred.cpp
//
// Deferred jobs: completion from any thread, no thread held while awaiting,
// cancellation, timeout, late completion, observer and re-run behaviour.

#include <array>
#include <atomic>
#include <chrono>
#include <doctest.h>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "sub0pipeline/deadline.hpp"
#include "test_helpers.hpp"

using namespace sub0pipeline;
using namespace std::chrono_literals;

namespace
{

// Bounded so a broken implementation fails the check instead of hanging the suite.
bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds limit = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Collects the handles that deferred bodies hand over, for a test thread to complete.
class Parked
{
public:
    void park(Completion done)
    {
        std::lock_guard lock{mtx_};
        handles_.push_back(std::move(done));
    }
    [[nodiscard]] std::size_t count()
    {
        std::lock_guard lock{mtx_};
        return handles_.size();
    }
    [[nodiscard]] std::vector<Completion> take()
    {
        std::lock_guard lock{mtx_};
        return std::move(handles_);
    }
private:
    std::mutex mtx_;
    std::vector<Completion> handles_;
};

struct EventLog final : IObserver
{
    std::mutex mtx;
    std::vector<std::string> events;
    std::string failureMessage;

    void add(std::string event)
    {
        std::lock_guard lock{mtx};
        events.push_back(std::move(event));
    }
    void onJobStart(RunId, JobId, std::string_view name) override
    {
        add("start:" + std::string{name});
    }
    void onJobAwaiting(RunId, JobId, std::string_view name) override
    {
        add("awaiting:" + std::string{name});
    }
    void onJobFinish(RunId, JobId, std::string_view name, JobStatus, float) override
    {
        add("finish:" + std::string{name});
    }
    void onJobFailure(RunId, JobId, std::string_view, PipelineError,
                      std::string_view message) noexcept override
    {
        std::lock_guard lock{mtx};
        failureMessage = std::string{message};
    }
};

// Fixed slots and a manually advanced clock, as in test_deadline.cpp.
class StepClock final : public IDeadlineService
{
public:
    bool arm(Deadline& deadline, std::chrono::milliseconds delay) noexcept override
    {
        std::lock_guard lock{mtx_};
        for (auto& slot : slots_)
        {
            if (!slot.deadline)
            {
                slot = {&deadline, now_ + delay};
                ++arms;
                return true;
            }
        }
        return false;
    }
    void cancelAndWait(Deadline& deadline) noexcept override
    {
        std::lock_guard lock{mtx_};
        for (auto& slot : slots_) if (slot.deadline == &deadline) slot = {};
    }
    void advance(std::chrono::milliseconds delta)
    {
        std::vector<Deadline*> due;
        {
            std::lock_guard lock{mtx_};
            now_ += delta;
            for (auto& slot : slots_)
            {
                if (slot.deadline && slot.at <= now_) due.push_back(std::exchange(slot.deadline, nullptr));
            }
        }
        for (auto* deadline : due) deadline->expire();
    }
    [[nodiscard]] int used()
    {
        std::lock_guard lock{mtx_};
        int n = 0;
        for (auto& slot : slots_) n += slot.deadline ? 1 : 0;
        return n;
    }
    int arms = 0;
private:
    struct Slot { Deadline* deadline = nullptr; std::chrono::milliseconds at{}; };
    std::mutex mtx_;
    std::array<Slot, 4> slots_{};
    std::chrono::milliseconds now_{};
};

} // namespace

TEST_CASE("Deferred: completion from another thread releases dependants only then")
{
    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    Parked parked;
    std::atomic<bool> dependantRan{false};
    auto fetch = pipe.emplaceDeferred([&](Completion done) { parked.park(std::move(done)); }).name("fetch");
    auto use = pipe.emplace([&] { dependantRan = true; }).name("use").succeed(fetch);

    std::atomic<bool> ranEarly{true};
    std::jthread completer{[&] {
        if (!waitUntil([&] { return parked.count() == 1U; })) return;
        CHECK(waitUntil([&] { return pipe.status(fetch) == JobStatus::kAwaiting; }));
        std::this_thread::sleep_for(10ms);
        ranEarly = dependantRan.load();
        for (auto& done : parked.take()) done.succeed();
    }};
    auto result = pipe.run(pool);
    completer.join();
    CHECK(result.has_value());
    CHECK_FALSE(ranEarly.load());
    CHECK(dependantRan.load());
    CHECK(pipe.status(fetch) == JobStatus::kDone);
    CHECK(pipe.status(use) == JobStatus::kDone);
}

TEST_CASE("Deferred: more awaiting jobs than pool threads while CPU jobs still finish")
{
    constexpr int kDeferred = 8;
    constexpr int kCpu = 8;
    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    Parked parked;
    std::atomic<int> cpuDone{0};
    std::atomic<int> afterDeferred{0};
    for (int i = 0; i < kDeferred; ++i)
    {
        auto d = pipe.emplaceDeferred([&](Completion done) { parked.park(std::move(done)); });
        (void)pipe.emplace([&] { ++afterDeferred; }).succeed(d);
    }
    for (int i = 0; i < kCpu; ++i) (void)pipe.emplace([&] { ++cpuDone; });

    std::atomic<bool> cpuFinishedWhileAwaiting{false};
    std::jthread completer{[&] {
        const bool allParked = waitUntil([&] { return parked.count() == kDeferred; });
        cpuFinishedWhileAwaiting = allParked && waitUntil([&] { return cpuDone.load() == kCpu; })
                                   && afterDeferred.load() == 0;
        for (auto& done : parked.take()) done.succeed();
    }};
    auto result = pipe.run(pool);
    completer.join();
    CHECK(result.has_value());
    CHECK(cpuFinishedWhileAwaiting.load());
    CHECK(afterDeferred.load() == kDeferred);
}

TEST_CASE("Deferred: threaded finish runs on an executor thread, not the completing thread")
{
    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    Parked parked;
    std::thread::id completerId;
    std::thread::id dependantId;
    auto d = pipe.emplaceDeferred([&](Completion done) { parked.park(std::move(done)); });
    (void)pipe.emplace([&] { dependantId = std::this_thread::get_id(); }).succeed(d);
    std::jthread completer{[&] {
        completerId = std::this_thread::get_id();
        if (waitUntil([&] { return parked.count() == 1U; }))
            for (auto& done : parked.take()) done.succeed();
    }};
    REQUIRE(pipe.run(pool).has_value());
    completer.join();
    CHECK(dependantId != std::thread::id{});
    CHECK(dependantId != completerId);
}

TEST_CASE("Deferred: completion before the body returns finishes without awaiting")
{
    Pipeline pipe;
    EventLog log;
    bool dependantRan = false;
    auto d = pipe.emplaceDeferred([](Completion done) { done.succeed(); }).name("d");
    (void)pipe.emplace([&] { dependantRan = true; }).name("next").succeed(d);
    REQUIRE(pipe.runInline(&log).has_value());
    CHECK(dependantRan);
    CHECK(pipe.status(d) == JobStatus::kDone);
    CHECK(log.events == std::vector<std::string>{"start:d", "finish:d", "start:next", "finish:next"});
}

TEST_CASE("Deferred: inline run waits on the run thread and reports awaiting in order")
{
    Pipeline pipe;
    EventLog log;
    std::jthread worker;
    std::thread::id runThread = std::this_thread::get_id();
    std::thread::id dependantThread;
    auto d = pipe.emplaceDeferred([&](Completion done) {
        worker = std::jthread{[done = std::move(done)]() mutable {
            std::this_thread::sleep_for(5ms);
            done.succeed();
        }};
    }).name("d");
    (void)pipe.emplace([&] { dependantThread = std::this_thread::get_id(); }).name("next").succeed(d);
    REQUIRE(pipe.runInline(&log).has_value());
    CHECK(dependantThread == runThread);
    CHECK(log.events == std::vector<std::string>{
        "start:d", "awaiting:d", "finish:d", "start:next", "finish:next"});
}

TEST_CASE("Deferred: snapshot and status show kAwaiting while pending")
{
    PriorityExecutor pool{{.threadCount = 1}};
    Pipeline pipe;
    Parked parked;
    auto d = pipe.emplaceDeferred([&](Completion done) { parked.park(std::move(done)); }).name("io");
    std::atomic<bool> sawAwaiting{false};
    std::jthread completer{[&] {
        if (waitUntil([&] { return parked.count() == 1U; }))
        {
            sawAwaiting = waitUntil([&] {
                auto snap = pipe.snapshot();
                return snap.size() == 1U && snap[0].status == JobStatus::kAwaiting;
            });
        }
        for (auto& done : parked.take()) done.succeed();
    }};
    REQUIRE(pipe.run(pool).has_value());
    completer.join();
    CHECK(sawAwaiting.load());
    CHECK(pipe.status(d) == JobStatus::kDone);
}

TEST_CASE("Deferred: error completion reports its message and firstFailureName")
{
    Pipeline pipe;
    EventLog log;
    auto d = pipe.emplaceDeferred([](Completion done) {
        done.fail(PipelineError::kJobFailed, "device busy");
    }).name("upload");
    auto next = pipe.emplace([] {}).name("next").succeed(d);
    auto result = pipe.runInline(&log);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
    CHECK(pipe.firstFailureName() == "upload");
    CHECK(log.failureMessage == "device busy");
    CHECK(pipe.status(d) == JobStatus::kFailed);
    CHECK(pipe.status(next) == JobStatus::kSkipped);
}

TEST_CASE("Deferred: an abandoned Completion fails the job")
{
    Pipeline pipe;
    EventLog log;
    auto d = pipe.emplaceDeferred([](Completion) {}).name("dropped");
    auto result = pipe.runInline(&log);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
    CHECK(pipe.status(d) == JobStatus::kFailed);
    CHECK(log.failureMessage == "completion abandoned");
}

TEST_CASE("Deferred: an optional deferred failure lets dependants run")
{
    Pipeline pipe;
    bool dependantRan = false;
    auto d = pipe.emplaceDeferred([](Completion done) { done.fail(PipelineError::kJobFailed); }).optional();
    (void)pipe.emplace([&] { dependantRan = true; }).succeed(d);
    CHECK(pipe.runInline().has_value());
    CHECK(dependantRan);
    CHECK(pipe.status(d) == JobStatus::kFailed);
}

TEST_CASE("Deferred: external stop while awaiting finishes the job; late completion releases the orphan")
{
    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    Parked parked;
    std::atomic<bool> cancelSeen{false};
    std::optional<std::stop_callback<std::function<void()>>> onCancel;
    auto d = pipe.emplaceDeferred([&](Completion done) {
        onCancel.emplace(done.stopToken(), std::function<void()>{[&] { cancelSeen = true; }});
        parked.park(std::move(done));
    }).name("remote");
    auto next = pipe.emplace([] {}).succeed(d);

    std::stop_source stop;
    std::jthread canceller{[&] {
        if (waitUntil([&] { return pipe.status(d) == JobStatus::kAwaiting; })) stop.request_stop();
    }};
    auto result = pipe.run(pool, stop.get_token());
    canceller.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCancelled);
    CHECK(cancelSeen.load());
    CHECK(pipe.status(d) == JobStatus::kCancelled);
    CHECK(pipe.status(next) == JobStatus::kSkipped);
    CHECK(pipe.firstFailureName() == "remote");
    CHECK(pipe.hasPendingOrphans());

    onCancel.reset();
    auto late = parked.take();
    REQUIRE(late.size() == 1U);
    late[0].succeed(); // ignored, apart from releasing the orphan
    CHECK_FALSE(late[0].valid());
    CHECK_FALSE(pipe.hasPendingOrphans());
    CHECK(pipe.status(d) == JobStatus::kCancelled);
    CHECK_FALSE(pipe.joinOrphans());
}

TEST_CASE("Deferred: Job::cancel while awaiting on an inline run")
{
    Pipeline pipe;
    Parked parked;
    auto d = pipe.emplaceDeferred([&](Completion done) { parked.park(std::move(done)); });
    std::jthread canceller{[&] {
        if (waitUntil([&] { return pipe.status(d) == JobStatus::kAwaiting; })) d.cancel();
    }};
    auto result = pipe.runInline();
    canceller.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCancelled);
    CHECK(pipe.status(d) == JobStatus::kCancelled);
    CHECK(pipe.hasPendingOrphans());
    parked.take().clear(); // destroying the handle also releases the orphan
    CHECK_FALSE(pipe.hasPendingOrphans());
}

TEST_CASE("Deferred: the next run joins a cancelled run's late completion first")
{
    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    Parked parked;
    int runs = 0;
    auto d = pipe.emplaceDeferred([&](Completion done) {
        ++runs;
        if (runs == 1) parked.park(std::move(done));
        else done.succeed();
    });
    std::stop_source stop;
    std::jthread canceller{[&] {
        if (waitUntil([&] { return pipe.status(d) == JobStatus::kAwaiting; })) stop.request_stop();
    }};
    CHECK_FALSE(pipe.run(pool, stop.get_token()).has_value());
    canceller.join();
    REQUIRE(pipe.hasPendingOrphans());

    std::atomic<bool> released{false};
    std::jthread late{[&] {
        std::this_thread::sleep_for(20ms);
        released = true;
        for (auto& done : parked.take()) done.succeed();
    }};
    CHECK(pipe.run(pool).has_value());
    CHECK(released.load());
    CHECK(runs == 2);
    CHECK(pipe.status(d) == JobStatus::kDone);
}

TEST_CASE("Deferred: native timeout while awaiting reports kTimeout")
{
    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    Parked parked;
    auto d = pipe.emplaceDeferred([&](Completion done) { parked.park(std::move(done)); })
                 .name("slow").timeout(20ms);
    auto next = pipe.emplace([] {}).succeed(d);
    auto result = pipe.run(pool);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kTimeout);
    CHECK(pipe.status(d) == JobStatus::kTimedOut);
    CHECK(pipe.status(next) == JobStatus::kSkipped);
    CHECK(pipe.hasPendingOrphans());
    parked.take().clear();
    CHECK_FALSE(pipe.hasPendingOrphans());
}

TEST_CASE("Deferred: work that answers a timeout with kCancelled still reports kTimeout")
{
    StepClock clock;
    Pipeline pipe;
    pipe.setDeadlineService(&clock);
    std::optional<std::stop_callback<std::function<void()>>> onStop;
    Completion held;
    auto d = pipe.emplaceDeferred([&](Completion done) {
        held = std::move(done);
        onStop.emplace(held.stopToken(), std::function<void()>{[&] {
            held.fail(PipelineError::kCancelled);
        }});
    }).timeout(10ms);
    std::jthread ticker{[&] {
        if (waitUntil([&] { return pipe.status(d) == JobStatus::kAwaiting; })) clock.advance(10ms);
    }};
    auto result = pipe.runInline();
    ticker.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kTimeout);
    CHECK(pipe.status(d) == JobStatus::kTimedOut);
    onStop.reset();
    CHECK_FALSE(pipe.hasPendingOrphans());
    CHECK(clock.used() == 0);
}

TEST_CASE("Deferred: an injected deadline is released when the job completes in time")
{
    StepClock clock;
    Pipeline pipe;
    pipe.setDeadlineService(&clock);
    PriorityExecutor pool{{.threadCount = 2}};
    Parked parked;
    auto d = pipe.emplaceDeferred([&](Completion done) { parked.park(std::move(done)); }).timeout(10ms);
    std::jthread completer{[&] {
        if (waitUntil([&] { return parked.count() == 1U; }))
        {
            clock.advance(9ms);
            for (auto& done : parked.take()) done.succeed();
        }
    }};
    CHECK(pipe.run(pool).has_value());
    completer.join();
    CHECK(pipe.status(d) == JobStatus::kDone);
    CHECK(clock.arms == 1);
    CHECK(clock.used() == 0);
    clock.advance(100ms); // a released registration must not touch the job
    CHECK(pipe.status(d) == JobStatus::kDone);
}

TEST_CASE("Deferred: re-runs start each run with fresh state")
{
    PriorityExecutor pool{{.threadCount = 2}};
    Pipeline pipe;
    std::atomic<int> dependants{0};
    std::vector<std::jthread> workers; // one body per run, so runs never share it
    auto d = pipe.emplaceDeferred([&](Completion done) {
        workers.emplace_back([done = std::move(done)]() mutable { done.succeed(); });
    });
    (void)pipe.emplace([&] { ++dependants; }).succeed(d);
    for (int i = 0; i < 20; ++i)
    {
        REQUIRE(pipe.run(pool).has_value());
        CHECK(pipe.status(d) == JobStatus::kDone);
    }
    CHECK(dependants.load() == 20);
    for (int i = 0; i < 5; ++i) REQUIRE(pipe.runInline().has_value());
    CHECK(dependants.load() == 25);
}

TEST_CASE("Deferred: an empty Completion is inert")
{
    Completion empty;
    CHECK_FALSE(empty.valid());
    CHECK_FALSE(empty.stopToken().stop_possible());
    empty.succeed();
    empty.fail(PipelineError::kJobFailed, "ignored");
    Completion moved{std::move(empty)};
    CHECK_FALSE(moved.valid());
}
