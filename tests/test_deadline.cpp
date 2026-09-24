#include "doctest.h"
#include "test_helpers.hpp"
#include <sub0pipeline/deadline.hpp>
#include <sub0pipeline/run_scope.hpp>
#include <array>
#include <condition_variable>
#include <latch>

using namespace sub0pipeline;
using namespace std::chrono_literals;

// Fixed slots and a manually advanced monotonic clock. Single-thread test
// service: no wall-clock sleeps, allocations, timer threads or event loop.
class ManualClock final : public IDeadlineService {
public:
    bool arm(Deadline& deadline, std::chrono::milliseconds delay) noexcept override {
        ++arms;
        for (auto& slot : slots) {
            if (!slot.deadline) {
                slot = {&deadline, now + delay};
                advance(0ms);
                return true;
            }
        }
        return false;
    }
    void cancel_and_wait(Deadline& deadline) noexcept override {
        for (auto& slot : slots) if (slot.deadline == &deadline) slot = {};
    }
    void advance(std::chrono::milliseconds delta) {
        now += delta;
        for (auto& slot : slots) {
            if (slot.deadline && slot.at <= now) {
                auto* deadline = std::exchange(slot.deadline, nullptr);
                deadline->expire();
            }
        }
    }
    int arms = 0;
private:
    struct Slot { Deadline* deadline = nullptr; std::chrono::milliseconds at{}; };
    std::array<Slot, 2> slots{};
    std::chrono::milliseconds now{};
};

TEST_CASE("Deadline: exact boundary, status, successor suppression and clean retry") {
    ManualClock clock;
    Pipeline pipe;
    pipe.set_deadline_service(&clock);
    bool expire = true;
    int acks = 0;
    auto commit = pipe.emplace([&](std::stop_token token) -> std::expected<void, PipelineError> {
        clock.advance(9ms);
        CHECK_FALSE(token.stop_requested());
        if (expire) clock.advance(1ms);
        CHECK(token.stop_requested() == expire);
        return {}; // An ignored timeout must not become success.
    }).timeout(10ms);
    auto ack = pipe.emplace([&] { ++acks; }).succeed(commit);
    auto result = pipe.run_inline();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kTimeout);
    CHECK(pipe.status(commit) == JobStatus::kTimedOut);
    CHECK(pipe.status(ack) == JobStatus::kSkipped);
    CHECK(acks == 0);
    expire = false;
    CHECK(pipe.run_inline().has_value());
    clock.advance(100ms); // Cancelled registrations must not touch destroyed stack state.
    CHECK(acks == 1);
    CHECK(clock.arms == 2);
}

TEST_CASE("Deadline: immediate expiry suppresses plain and cooperative bodies") {
    for (bool cooperative : {false, true}) {
        ManualClock clock;
        Pipeline pipe;
        pipe.set_deadline_service(&clock);
        bool ran = false;
        auto job = cooperative
            ? pipe.emplace([&](std::stop_token) -> std::expected<void, PipelineError> { ran = true; return {}; })
            : pipe.emplace([&] { ran = true; });
        job.timeout(0ms);
        auto result = pipe.run_inline();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == PipelineError::kTimeout);
        CHECK_FALSE(ran);
        CHECK_FALSE(pipe.has_pending_orphans());
    }
}

TEST_CASE("Deadline: registration exhaustion fails closed and untimed work skips service") {
    struct Full final : IDeadlineService {
        bool arm(Deadline&, std::chrono::milliseconds) noexcept override { return false; }
        void cancel_and_wait(Deadline&) noexcept override { CHECK(false); }
    } full;
    Pipeline pipe;
    pipe.set_deadline_service(&full);
    int ran = 0;
    auto job = pipe.emplace([&] { ++ran; });
    CHECK(pipe.run_inline().has_value());
    job.timeout(10ms);
    auto result = pipe.run_inline();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kDeadlineUnavailable);
    CHECK(ran == 1);
}

TEST_CASE("Deadline: external cancellation stays cancellation, on-demand uses same timer") {
    ManualClock clock;
    Pipeline pipe;
    pipe.set_deadline_service(&clock);
    QueuedExecutor executor;
    std::stop_source stop;
    auto job = pipe.emplace([&](std::stop_token token) -> std::expected<void, PipelineError> {
        stop.request_stop();
        CHECK(token.stop_requested());
        return std::unexpected(PipelineError::kCancelled);
    }).timeout(1h);
    auto result = pipe.run(executor, stop.get_token());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kCancelled);
    auto event = pipe.add_on_demand([&](std::stop_token) -> std::expected<void, PipelineError> {
        clock.advance(10ms);
        return {};
    }).timeout(10ms);
    pipe.arm(executor);
    REQUIRE(pipe.trigger(event).has_value());
    executor.wait_all();
    CHECK(pipe.status(event) == JobStatus::kTimedOut);
    CHECK(clock.arms == 2);
}

TEST_CASE("RunScope: destruction releases cooperative I/O before borrowed members") {
    InlineExecutor executor;
    std::latch entered{1};
    int record = 42;
    bool returned = false;
    Pipeline pipe;
    (void)pipe.emplace([&](std::stop_token token) -> std::expected<void, PipelineError> {
        std::mutex mutex;
        std::condition_variable_any ready;
        std::unique_lock lock{mutex};
        entered.count_down();
        ready.wait(lock, token, [] { return false; });
        CHECK(record == 42);
        returned = true;
        return std::unexpected(PipelineError::kCancelled);
    });
    {
        RunScope scope{pipe, executor};
        entered.wait();
        CHECK_FALSE(scope.complete());
    }
    CHECK(returned);
}

TEST_CASE("RunScope: completion includes non-cooperative timeout work") {
    InlineExecutor executor;
    Pipeline pipe;
    std::latch timedOut{1}, release{1};
    struct Observer final : IObserver {
        std::latch& timedOut;
        explicit Observer(std::latch& latch) : timedOut{latch} {}
        void onStart(std::string_view) override {}
        void onFinish(std::string_view, JobStatus status, float) override {
            if (status == JobStatus::kTimedOut) timedOut.count_down();
        }
    } observer{timedOut};
    bool finished = false;
    (void)pipe.emplace([&] { release.wait(); finished = true; }).timeout(0ms);
    RunScope scope{pipe, executor, &observer};
    timedOut.wait();
    CHECK_FALSE(scope.complete());
    release.count_down();
    auto result = scope.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kTimeout);
    CHECK(scope.complete());
    CHECK(finished);
    CHECK_FALSE(pipe.has_pending_orphans());
}

TEST_CASE("On-demand: cancellation resets for retry and duplicate queueing is rejected") {
    Pipeline pipe, other;
    QueuedExecutor executor;
    int ran = 0;
    auto event = pipe.add_on_demand([&]() -> std::expected<void, PipelineError> { ++ran; return {}; });
    auto foreign = other.add_on_demand([]() -> std::expected<void, PipelineError> { return {}; });
    pipe.arm(executor);
    CHECK(pipe.trigger(foreign).error() == PipelineError::kUnknownJob);
    REQUIRE(pipe.trigger(event).has_value());
    CHECK(pipe.trigger(event).error() == PipelineError::kBusy);
    event.cancel();
    executor.wait_all();
    CHECK(ran == 0);
    REQUIRE(pipe.trigger(event).has_value());
    executor.wait_all();
    CHECK(ran == 1);
}

// Explicitly driven concurrent service: cancellation drains in-flight expiry.
class ControlledClock final : public IDeadlineService {
public:
    std::latch armed{1}, cancelling{1};
    bool arm(Deadline& deadline, std::chrono::milliseconds) noexcept override {
        std::lock_guard lock{mutex};
        slot = &deadline;
        armed.count_down();
        return true;
    }
    void cancel_and_wait(Deadline&) noexcept override {
        std::unique_lock lock{mutex};
        slot = nullptr;
        cancelling.count_down();
        idle.wait(lock, [&] { return active == 0; });
    }
    void expire() {
        Deadline* deadline;
        {
            std::lock_guard lock{mutex};
            deadline = slot;
            if (!deadline) return;
            ++active;
        }
        deadline->expire();
        std::lock_guard lock{mutex};
        --active;
        idle.notify_all();
    }
private:
    std::mutex mutex;
    std::condition_variable idle;
    Deadline* slot = nullptr;
    int active = 0;
};

TEST_CASE("Deadline: completion drains a concurrently executing stop callback") {
    ControlledClock clock;
    InlineExecutor executor;
    Pipeline pipe;
    pipe.set_deadline_service(&clock);
    std::latch entered{1}, callbackEntered{1}, releaseCallback{1}, bodyMayReturn{1};
    std::atomic<bool> touchedBorrowed{false};
    (void)pipe.emplace([&](std::stop_token token) -> std::expected<void, PipelineError> {
        std::stop_callback callback{token, [&] {
            callbackEntered.count_down();
            releaseCallback.wait();
            touchedBorrowed = true;
        }};
        entered.count_down();
        bodyMayReturn.wait();
        return {};
    }).timeout(10ms);
    RunScope scope{pipe, executor};
    entered.wait();
    std::jthread timer{[&] { clock.expire(); }};
    callbackEntered.wait();
    bodyMayReturn.count_down();
    CHECK_FALSE(scope.complete());
    releaseCallback.count_down();
    timer.join();
    auto result = scope.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kTimeout);
    CHECK(touchedBorrowed.load());
    CHECK(scope.complete());
}

TEST_CASE("Deadline: plain blocked worker remains owned after injected expiry") {
    ControlledClock clock;
    Pipeline pipe;
    pipe.set_deadline_service(&clock);
    std::latch entered{1}, release{1};
    bool finished = false;
    (void)pipe.emplace([&] { entered.count_down(); release.wait(); finished = true; }).timeout(10ms);
    std::expected<void, PipelineError> result;
    std::jthread runner{[&] { result = pipe.run_inline(); }};
    entered.wait();
    clock.expire();
    runner.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kTimeout);
    CHECK(pipe.has_pending_orphans());
    release.count_down();
    pipe.join_orphans();
    CHECK(finished);
    CHECK_FALSE(pipe.has_pending_orphans());
}

TEST_CASE("Deadline: plain completion unregisters without waiting for expiry") {
    ControlledClock clock;
    Pipeline pipe;
    pipe.set_deadline_service(&clock);
    (void)pipe.emplace([] {}).timeout(1h);
    CHECK(pipe.run_inline().has_value());
    clock.expire(); // no access to the destroyed registration
    CHECK_FALSE(pipe.has_pending_orphans());
}
