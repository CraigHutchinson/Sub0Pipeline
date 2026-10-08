#include <atomic>
#include <doctest.h>
#include <expected>
#include <functional>
#include <latch>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "sub0pipeline/sub0pipeline.hpp"

using namespace sub0pipeline;

namespace
{
class FailureObserver final : public IObserver
{
public:
    void onJobFinish(RunId, JobId, std::string_view, JobStatus, float) override
    {
        ++finished_;
    }
    void onJobFailure(RunId, JobId, std::string_view name, PipelineError error,
                      std::string_view) noexcept override
    {
        if (name == "reject" && error == PipelineError::kJobFailed) ++rejected_;
    }
    std::atomic<int> finished_{0};
    std::atomic<int> rejected_{0};
};

/** Rejects one named submission while preserving accepted work on a real executor. */
class RejectingExecutor final : public IExecutor
{
public:
    explicit RejectingExecutor(std::string_view rejectedName) : rejectedName_{rejectedName} {}
    ~RejectingExecutor() override { parent_.waitAll(); }

    void dispatch(std::string_view name, std::function<void()> fn,
                  std::function<void()> onComplete, int affinity,
                  uint8_t priority, uint32_t stack) override
    {
        if (reject_.load() && name == rejectedName_)
        {
            if (beforeReject_) beforeReject_->wait();
            rejected_.count_down();
            throw std::bad_alloc{};
        }
        const bool hold = name == heldCompletionName_;
        parent_.dispatch(name, std::move(fn),
            [this, callback = std::move(onComplete), hold]
            {
                if (callback) callback();
                if (hold)
                {
                    completionEntered_.count_down();
                    releaseCompletion_.wait();
                }
            }, affinity, priority, stack);
    }

    void waitAll() override
    {
        if (!waitSeen_.exchange(true)) waitEntered_.count_down();
        parent_.waitAll();
    }

    [[nodiscard]] int concurrency() const noexcept override { return 2; }

    DesktopExecutor parent_;
    std::atomic<bool> reject_{true};
    std::latch* beforeReject_{nullptr}; // non-owning, fixture lifetime
    std::string_view heldCompletionName_;
    std::latch rejected_{1};
    std::latch waitEntered_{1};
    std::latch completionEntered_{1};
    std::latch releaseCompletion_{1};

private:
    std::string_view rejectedName_;
    std::atomic<bool> waitSeen_{false};
};

struct StartCopyState
{
    bool armed_{false};
    int copies_{0};
    std::atomic<int> alive_{0};
    std::latch firstStarted_{1};
};

/** The existing setup hook permits copy failure without a test-only thread factory. */
struct ThrowingStartCopy
{
    explicit ThrowingStartCopy(StartCopyState& state) noexcept : state_{state} {}
    ThrowingStartCopy(const ThrowingStartCopy& other) : state_{other.state_}
    {
        if (state_.armed_ && ++state_.copies_ == 2)
        {
            state_.firstStarted_.wait();
            throw std::bad_alloc{};
        }
    }
    void operator()() const noexcept
    {
        ++state_.alive_;
        state_.firstStarted_.count_down();
    }
    StartCopyState& state_;
};

struct DestructionGate
{
    std::latch entered_{1};
    std::latch release_{1};
    std::atomic<bool> finished_{false};
};

/** Owns state whose reclamation must precede the executor's join publication. */
struct GatedPayload
{
    explicit GatedPayload(DestructionGate& gate) noexcept : gate_{gate} {}
    ~GatedPayload()
    {
        gate_.entered_.count_down();
        gate_.release_.wait();
        gate_.finished_ = true;
    }

    DestructionGate& gate_;
    int value_{17};
};

/** A completion target can borrow body-owned state until its own destruction. */
struct BorrowingCompletion
{
    std::weak_ptr<int> body_;
    std::atomic<bool>& violation_;
    std::atomic<int>& calls_;

    ~BorrowingCompletion()
    {
        if (body_.expired())
        {
            violation_ = true;
        }
    }

    void operator()() const noexcept { ++calls_; }
};

/** Retains parent wrapper storage until its own join, independently of a scope. */
class RetainingExecutor final : public IExecutor
{
public:
    void dispatch(std::string_view, std::function<void()> fn,
                  std::function<void()> onComplete, int, uint8_t, uint32_t) override
    {
        fn_ = std::move(fn);
        onComplete_ = std::move(onComplete);
    }

    void runAccepted()
    {
        fn_();
        if (onComplete_)
        {
            onComplete_();
        }
    }

    void waitAll() override
    {
        onComplete_ = {};
        fn_ = {};
    }

    [[nodiscard]] int concurrency() const noexcept override { return 1; }

    std::function<void()> fn_;
    std::function<void()> onComplete_;
};
}

TEST_CASE("Submission: bounded pool rejects a full queue and remains reusable")
{
    PriorityExecutor executor{{.threadCount = 1, .queueCapacity = 1}};
    std::latch entered{1}, release{1};
    std::atomic<int> bodies{0}, completions{0}, rejectedCalls{0};
    executor.dispatch("held", [&] { ++bodies; entered.count_down(); release.wait(); },
                      [&] { ++completions; }, -1, 5, 0);
    entered.wait();
    executor.dispatch("queued", [&] { ++bodies; }, [&] { ++completions; }, -1, 5, 0);
    CHECK_THROWS_AS(executor.dispatch("rejected", [&] { ++rejectedCalls; },
                                     [&] { ++rejectedCalls; }, -1, 5, 0), std::runtime_error);
    auto payload = std::make_shared<int>(17);
    const std::weak_ptr<int> retained = payload;
    CHECK_THROWS_AS(executor.dispatch("resource", [payload] {}, [payload] {}, -1, 5, 0),
                    std::runtime_error);
    payload.reset();
    CHECK(retained.expired());
    release.count_down();
    executor.waitAll();
    CHECK(bodies.load() == 2);
    CHECK(completions.load() == 2);
    CHECK(rejectedCalls.load() == 0);
    executor.dispatch("reused", [&] { ++bodies; }, [&] { ++completions; }, -1, 5, 0);
    executor.waitAll();
    CHECK(bodies.load() == 3);
    CHECK(completions.load() == 3);
}

TEST_CASE("Submission: bounded pool retains queued priority ordering")
{
    PriorityExecutor executor{{.threadCount = 1, .queueCapacity = 2}};
    std::latch entered{1}, release{1};
    int sequence = 0, low = 0, high = 0;
    executor.dispatch("held", [&] { entered.count_down(); release.wait(); }, {}, -1, 5, 0);
    entered.wait();
    executor.dispatch("low", [&] { low = ++sequence; }, {}, -1, 1, 0);
    executor.dispatch("high", [&] { high = ++sequence; }, {}, -1, 24, 0);
    release.count_down();
    executor.waitAll();
    CHECK(high == 1);
    CHECK(low == 2);
}

TEST_CASE("Submission: pool join includes accepted target destruction")
{
    DestructionGate gate;
    PriorityExecutor executor{{.threadCount = 1, .queueCapacity = 1}};
    std::latch bodyEntered{1}, releaseBody{1}, waiterEntered{1};
    auto payload = std::make_shared<GatedPayload>(gate);
    const std::weak_ptr<GatedPayload> bodyRetained = payload;
    auto completionPayload = std::make_shared<int>(23);
    const std::weak_ptr<int> completionRetained = completionPayload;
    const auto* borrowed = payload.get();
    std::atomic<bool> completionRead{false}, returned{false}, reclaimedAtJoin{false};
    executor.dispatch("owned", [payload, &bodyEntered, &releaseBody]
    {
        bodyEntered.count_down();
        releaseBody.wait();
    }, [borrowed, completionPayload, &completionRead]
    {
        completionRead = borrowed->value_ == 17 && *completionPayload == 23;
    }, -1, 5, 0);
    bodyEntered.wait();
    payload.reset();
    completionPayload.reset();
    std::jthread waiter{[&]
    {
        waiterEntered.count_down();
        executor.waitAll();
        reclaimedAtJoin = gate.finished_.load() && completionRetained.expired();
        returned = true;
    }};
    waiterEntered.wait();
    releaseBody.count_down();
    gate.entered_.wait();
    CHECK_FALSE(returned.load());
    CHECK(completionRetained.expired());
    gate.release_.count_down();
    waiter.join();
    CHECK(completionRead.load());
    CHECK(reclaimedAtJoin.load());
    CHECK(bodyRetained.expired());
}

TEST_CASE("Submission: scoped join destroys targets without joining unrelated parent work")
{
    DestructionGate gate;
    PriorityExecutor parent{{.threadCount = 2, .queueCapacity = 2}};
    std::latch unrelatedEntered{1}, releaseUnrelated{1};
    std::atomic<bool> unrelatedFinished{false};
    parent.dispatch("unrelated", [&]
    {
        unrelatedEntered.count_down();
        releaseUnrelated.wait();
        unrelatedFinished = true;
    }, {}, -1, 5, 0);
    unrelatedEntered.wait();
    auto scope = std::make_unique<ScopedExecutor>(parent);
    std::latch bodyEntered{1}, releaseBody{1}, waiterEntered{1};
    auto payload = std::make_shared<GatedPayload>(gate);
    const std::weak_ptr<GatedPayload> retained = payload;
    const auto* borrowed = payload.get();
    std::atomic<bool> completionRead{false}, returned{false}, reclaimedAtJoin{false};
    scope->dispatch("owned", [payload, &bodyEntered, &releaseBody]
    {
        bodyEntered.count_down();
        releaseBody.wait();
    }, [borrowed, &completionRead] { completionRead = borrowed->value_ == 17; }, -1, 5, 0);
    bodyEntered.wait();
    payload.reset();
    std::jthread waiter{[&]
    {
        waiterEntered.count_down();
        scope->waitAll();
        reclaimedAtJoin = gate.finished_.load();
        returned = true;
    }};
    waiterEntered.wait();
    releaseBody.count_down();
    gate.entered_.wait();
    CHECK_FALSE(returned.load());
    gate.release_.count_down();
    waiter.join();
    CHECK(completionRead.load());
    CHECK(reclaimedAtJoin.load());
    CHECK(retained.expired());
    CHECK_FALSE(unrelatedFinished.load());
    scope.reset();
    releaseUnrelated.count_down();
    parent.waitAll();
    CHECK(unrelatedFinished.load());
}

TEST_CASE("Submission: completed scope releases original targets while parent retains wrappers")
{
    RetainingExecutor parent;
    auto scope = std::make_unique<ScopedExecutor>(parent);
    auto payload = std::make_shared<int>(17);
    const std::weak_ptr<int> bodyRetained = payload;
    auto completionPayload = std::make_shared<int>(23);
    const std::weak_ptr<int> completionRetained = completionPayload;
    const auto* borrowed = payload.get();
    bool completionRead = false;
    scope->dispatch("owned", [payload] {}, [borrowed, completionPayload, &completionRead]
    {
        completionRead = *borrowed == 17 && *completionPayload == 23;
    }, -1, 5, 0);
    payload.reset();
    completionPayload.reset();
    parent.runAccepted();
    scope->waitAll();
    CHECK(completionRead);
    CHECK(bodyRetained.expired());
    CHECK(completionRetained.expired());
    CHECK(static_cast<bool>(parent.fn_));
    CHECK(static_cast<bool>(parent.onComplete_));
    scope.reset();
    parent.waitAll();
}

TEST_CASE("Submission: completion target destruction retains body-owned state")
{
    const auto receive = [](IExecutor& executor)
    {
        std::atomic<bool> violation{false};
        std::atomic<int> calls{0};
        auto payload = std::make_shared<int>(17);
        const std::weak_ptr<int> retained = payload;
        std::function<void()> body = [payload] {};
        std::function<void()> completion = BorrowingCompletion{retained, violation, calls};
        payload.reset();
        // Moving a std::function may retain its source target. Clear caller ownership
        // before dispatch so the join observes only accepted executor-owned targets.
        executor.dispatch("borrowed", std::exchange(body, {}), std::exchange(completion, {}), -1, 5, 0);
        executor.waitAll();
        CHECK(calls.load() == 1);
        CHECK_FALSE(violation.load());
        CHECK(retained.expired());
    };
    SequentialExecutor inlineExecutor;
    receive(inlineExecutor);
    DesktopExecutor desktopExecutor;
    receive(desktopExecutor);
    PriorityExecutor pool{{.threadCount = 1, .queueCapacity = 1}};
    receive(pool);
    ScopedExecutor scope{pool};
    receive(scope);
}

TEST_CASE("Submission: pool validates count representations before launching workers")
{
    CHECK_THROWS_AS(PriorityExecutor(PriorityExecutor::Options{
        .threadCount = std::numeric_limits<unsigned int>::max()}), std::runtime_error);
    CHECK_THROWS_AS(PriorityExecutor(PriorityExecutor::Options{
        .threadCount = 1, .queueCapacity = std::numeric_limits<uint32_t>::max()}), std::runtime_error);
}

TEST_CASE("Submission: partial pool startup failure joins the started worker")
{
    StartCopyState state;
    PriorityExecutor::Options options{.threadCount = 3, .onThreadStart = ThrowingStartCopy{state}};
    state.armed_ = true;
    CHECK_THROWS_AS(PriorityExecutor(std::move(options)), std::bad_alloc);
    CHECK(state.alive_.load() == 1);
    CHECK(state.copies_ == 2);
}

TEST_CASE("Submission: scoped rejection rolls back and an inline parent completes safely")
{
    RejectingExecutor parent{"reject"};
    ScopedExecutor scoped{parent};
    int ran = 0;
    CHECK_THROWS_AS(scoped.dispatch("reject", [&] { ++ran; }, [&] { ++ran; }, -1, 5, 0),
                    std::bad_alloc);
    scoped.waitAll();
    CHECK(ran == 0);
    parent.reject_ = false;
    scoped.dispatch("accept", [&] { ++ran; }, [&] { ++ran; }, -1, 5, 0);
    scoped.waitAll();
    parent.waitAll();
    CHECK(ran == 2);

    SequentialExecutor inlineParent;
    ScopedExecutor inlineScope{inlineParent};
    inlineScope.dispatch("inline", [&] { ++ran; }, [&] { ++ran; }, -1, 5, 0);
    inlineScope.waitAll();
    CHECK(ran == 4);
}

TEST_CASE("Submission: rejected root joins accepted body and completion before returning")
{
    RejectingExecutor executor{"reject"};
    std::latch entered{1}, release{1};
    executor.beforeReject_ = &entered;
    executor.heldCompletionName_ = "held";
    Pipeline pipeline;
    bool firstRun = true;
    std::atomic<int> heldRuns{0}, otherRuns{0};
    auto held = pipeline.emplace([&]
    {
        ++heldRuns;
        if (firstRun)
        {
            entered.count_down();
            release.wait();
        }
    }).name("held");
    auto rejected = pipeline.emplace([&] { ++otherRuns; }).name("reject").optional();
    auto descendant = pipeline.emplace([&] { ++otherRuns; }).name("descendant");
    descendant.succeed(rejected);
    auto unsubmitted = pipeline.emplace([&] { ++otherRuns; }).name("unsubmitted");
    std::expected<void, PipelineError> result;
    std::atomic<bool> returned{false};
    FailureObserver observer;
    std::jthread runner{[&] { result = pipeline.run(executor, &observer); returned = true; }};
    executor.rejected_.wait();
    executor.waitEntered_.wait();
    CHECK_FALSE(returned.load());
    release.count_down();
    executor.completionEntered_.wait();
    CHECK_FALSE(returned.load());
    executor.releaseCompletion_.count_down();
    runner.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
    CHECK(pipeline.firstFailureName() == "reject");
    CHECK(pipeline.status(held) == JobStatus::kDone);
    CHECK(pipeline.status(rejected) == JobStatus::kFailed);
    CHECK(pipeline.status(descendant) == JobStatus::kSkipped);
    CHECK(pipeline.status(unsubmitted) == JobStatus::kSkipped);
    CHECK(otherRuns.load() == 0);
    CHECK(observer.finished_.load() == 4);
    CHECK(observer.rejected_.load() == 1);
    executor.reject_ = false;
    // One-shot fixture latches are not reached by the recovery run.
    firstRun = false;
    executor.heldCompletionName_ = {};
    CHECK(pipeline.run(executor).has_value());
    CHECK(heldRuns.load() == 2);
    CHECK(otherRuns.load() == 3);
    CHECK(pipeline.firstFailureName().empty());
}

TEST_CASE("Submission: rejected successor is contained on its worker and graph reruns")
{
    RejectingExecutor executor{"reject"};
    std::latch peerEntered{1}, releasePeer{1};
    executor.heldCompletionName_ = "peer";
    Pipeline pipeline;
    bool firstRun = true;
    auto root = pipeline.emplace([&] { peerEntered.wait(); }).name("root");
    std::atomic<int> dependentRuns{0};
    auto rejected = pipeline.emplace([&] { ++dependentRuns; }).name("reject").optional();
    rejected.succeed(root);
    auto descendant = pipeline.emplace([&] { ++dependentRuns; }).name("descendant");
    descendant.succeed(rejected);
    auto peer = pipeline.emplace([&]
    {
        if (firstRun)
        {
            peerEntered.count_down();
            releasePeer.wait();
        }
    }).name("peer");
    std::expected<void, PipelineError> result;
    std::atomic<bool> returned{false};
    FailureObserver observer;
    std::jthread runner{[&] { result = pipeline.run(executor, &observer); returned = true; }};
    executor.rejected_.wait();
    executor.waitEntered_.wait();
    CHECK_FALSE(returned.load());
    releasePeer.count_down();
    executor.completionEntered_.wait();
    CHECK_FALSE(returned.load());
    executor.releaseCompletion_.count_down();
    runner.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PipelineError::kJobFailed);
    CHECK(pipeline.status(rejected) == JobStatus::kFailed);
    CHECK(pipeline.status(descendant) == JobStatus::kSkipped);
    CHECK(pipeline.status(peer) == JobStatus::kDone);
    CHECK(dependentRuns.load() == 0);
    CHECK(pipeline.firstFailureName() == "reject");
    CHECK(observer.finished_.load() == 4);
    CHECK(observer.rejected_.load() == 1);
    firstRun = false;
    executor.heldCompletionName_ = {};
    executor.reject_ = false;
    CHECK(pipeline.run(executor).has_value());
    CHECK(dependentRuns.load() == 2);
}

TEST_CASE("Submission: rejected on-demand job fails without preventing retry")
{
    Pipeline pipeline;
    RejectingExecutor executor{"reject"};
    int ran = 0;
    auto job = pipeline.addOnDemand([&]() -> std::expected<void, PipelineError> { ++ran; return {}; })
        .name("reject").optional();
    pipeline.arm(executor);
    const auto failed = pipeline.trigger(job);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error() == PipelineError::kJobFailed);
    CHECK(pipeline.status(job) == JobStatus::kFailed);
    CHECK(ran == 0);
    executor.reject_ = false;
    CHECK(pipeline.trigger(job).has_value());
    executor.waitAll();
    CHECK(ran == 1);
    CHECK(pipeline.status(job) == JobStatus::kDone);
}
