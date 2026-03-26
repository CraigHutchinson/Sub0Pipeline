// src/sub0pipeline.cpp
//
// Sub0Pipeline — DAG scheduler: topological sort + parallel dispatch.
// Platform-agnostic core; execution strategy injected via IExecutor.
//
// GCC 15 + ESP-IDF defines _GLIBCXX_HAVE_POSIX_SEMAPHORE which causes
// <functional> → <semaphore> → <semaphore.h> include chain. newlib has
// no POSIX semaphores, so break the chain before any STL includes.
#ifdef _GLIBCXX_HAVE_POSIX_SEMAPHORE
#undef _GLIBCXX_HAVE_POSIX_SEMAPHORE
#endif
#ifdef _GLIBCXX_USE_POSIX_SEMAPHORE
#undef _GLIBCXX_USE_POSIX_SEMAPHORE
#endif

#include <sub0pipeline/sub0pipeline.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#if __has_include(<freertos/FreeRTOS.h>)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <thread>
#endif

#if __has_include(<trace/nn_trace.hpp>)
#include <trace/nn_trace.hpp>
#else
#define NN_TRACE_SCOPE(...)   ((void)0)
#define NN_TRACE_INSTANT(...) ((void)0)
#define NN_TRACE_COUNTER(...) ((void)0)
#endif

namespace sub0pipeline {

// ── Internal node ─────────────────────────────────────────────────────────────

struct Pipeline::Node
{
    std::string                                          nameStr_;
    std::function<std::expected<void, PipelineError>()> fn_;
    std::chrono::milliseconds                            timeout_{30'000};
    int                                                  coreAffinity_{-1};
    uint8_t                                              priority_{5U};
    uint32_t                                             stackBytes_{8192U};
    bool                                                 isOptional_{false};
    const char*                                          statusText_{nullptr};

    std::vector<uint32_t>   predecessors_;
    std::vector<uint32_t>   successors_;

    // std::atomic is non-copyable/non-movable by default, but std::vector may
    // reallocate nodes during the build phase. Loading in the move constructor
    // is safe because nodes are only moved before execution begins.
    std::atomic<JobStatus>  jobStatus_{JobStatus::kPending};
    std::atomic<uint32_t>   unmetDeps_{0U};

    Node() = default;

    Node(Node&& o) noexcept
        : nameStr_{std::move(o.nameStr_)}
        , fn_{std::move(o.fn_)}
        , timeout_{o.timeout_}
        , coreAffinity_{o.coreAffinity_}
        , priority_{o.priority_}
        , stackBytes_{o.stackBytes_}
        , isOptional_{o.isOptional_}
        , statusText_{o.statusText_}
        , predecessors_{std::move(o.predecessors_)}
        , successors_{std::move(o.successors_)}
        , jobStatus_{o.jobStatus_.load(std::memory_order_relaxed)}
        , unmetDeps_{o.unmetDeps_.load(std::memory_order_relaxed)}
    {}

    Node& operator=(Node&&)      = delete;
    Node(const Node&)            = delete;
    Node& operator=(const Node&) = delete;
};

// ── Pipeline implementation structure ─────────────────────────────────────────

struct Pipeline::Impl
{
    std::vector<Node>         nodes_;
    std::vector<TickJob>      ticks_;
    std::atomic<uint32_t>     completedCount_{0U}; ///< Incremented atomically from job threads.
};

// ── Job builder methods ───────────────────────────────────────────────────────

Job& Job::name(std::string_view n)
{
    if (pipeline_ && valid()) pipeline_->node(idx_).nameStr_ = std::string{n};
    return *this;
}

Job& Job::timeout(std::chrono::milliseconds t) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).timeout_ = t;
    return *this;
}

Job& Job::stack(uint32_t bytes) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).stackBytes_ = bytes;
    return *this;
}

Job& Job::core(int c) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).coreAffinity_ = c;
    return *this;
}

Job& Job::priority(uint8_t p) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).priority_ = p;
    return *this;
}

Job& Job::optional(bool opt) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).isOptional_ = opt;
    return *this;
}

Job& Job::status(const char* text) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).statusText_ = text;
    return *this;
}

Job& Job::succeed(Job other)
{
    if (!pipeline_ || !valid() || !other.valid()) return *this;
    auto& selfNode  = pipeline_->node(idx_);
    auto& otherNode = pipeline_->node(other.idx_);
    selfNode.predecessors_.push_back(other.idx_);
    otherNode.successors_.push_back(idx_);
    return *this;
}

Job& Job::precede(Job other)
{
    if (!pipeline_ || !valid() || !other.valid()) return *this;
    auto& selfNode  = pipeline_->node(idx_);
    auto& otherNode = pipeline_->node(other.idx_);
    selfNode.successors_.push_back(other.idx_);
    otherNode.predecessors_.push_back(idx_);
    return *this;
}

// ── Pipeline lifecycle ────────────────────────────────────────────────────────

Pipeline::Pipeline() : impl_{std::make_unique<Impl>()} {}
Pipeline::~Pipeline() = default;

// ── Internal node access ──────────────────────────────────────────────────────

Pipeline::Node& Pipeline::node(uint32_t idx)
{
    return impl_->nodes_[idx];
}

const Pipeline::Node& Pipeline::node(uint32_t idx) const
{
    return impl_->nodes_[idx];
}

// ── DAG construction ──────────────────────────────────────────────────────────

Job Pipeline::emplace(std::function<std::expected<void, PipelineError>()> fn)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    const auto idx = static_cast<uint32_t>(impl_->nodes_.size());
    impl_->nodes_.emplace_back();
    impl_->nodes_.back().fn_      = std::move(fn);
    impl_->nodes_.back().nameStr_ = "job_" + std::to_string(idx);
    return Job{idx, this};
}

Job Pipeline::emplace_void(std::function<void()> fn)
{
    std::function<std::expected<void, PipelineError>()> wrapper =
        [f = std::move(fn)]() -> std::expected<void, PipelineError>
        {
            f();
            return {};
        };
    return emplace(std::move(wrapper));
}

std::size_t Pipeline::size() const noexcept
{
    return impl_ ? impl_->nodes_.size() : 0U;
}

// ── Validation ────────────────────────────────────────────────────────────────

auto Pipeline::validate() const -> std::expected<void, PipelineError>
{
    if (!impl_) return {};

    const auto n = impl_->nodes_.size();

    // Kahn's algorithm: topological sort — cycle detected if not all nodes visited.
    std::vector<uint32_t> inDegree(n, 0U);
    for (const auto& nd : impl_->nodes_) {
        for (auto succ : nd.successors_) {
            ++inDegree[succ];
        }
    }

    std::queue<uint32_t> ready;
    for (uint32_t i = 0U; i < n; ++i) {
        if (inDegree[i] == 0U) ready.push(i);
    }

    uint32_t visited = 0U;
    while (!ready.empty()) {
        const auto idx = ready.front();
        ready.pop();
        ++visited;
        for (auto succ : impl_->nodes_[idx].successors_) {
            if (--inDegree[succ] == 0U) ready.push(succ);
        }
    }

    if (visited != static_cast<uint32_t>(n))
        return std::unexpected(PipelineError::kCyclicDependency);

    return {};
}

// ── Execution ─────────────────────────────────────────────────────────────────

auto Pipeline::run(IExecutor& executor, IObserver* observer)
    -> std::expected<void, PipelineError>
{
    if (!impl_ || impl_->nodes_.empty()) return {};

    if (auto v = validate(); !v) return v;

    const auto total = impl_->nodes_.size();
    impl_->completedCount_.store(0U, std::memory_order_relaxed);

    // Initialise per-node runtime state.
    for (auto& nd : impl_->nodes_) {
        nd.unmetDeps_.store(
            static_cast<uint32_t>(nd.predecessors_.size()),
            std::memory_order_relaxed);
        nd.jobStatus_.store(
            nd.predecessors_.empty() ? JobStatus::kReady : JobStatus::kPending,
            std::memory_order_relaxed);
    }

    // hasFatalFailure_ + fatalError_ are written from concurrent job threads.
    // Use atomic<bool> for the flag and a mutex to capture only the first error.
    std::atomic<bool> hasFatalFailure{false};
    PipelineError     fatalError{PipelineError::kJobFailed};
    std::mutex        fatalMtx;

    // skipNode: mark a node kSkipped and cascade transitively to all successors.
    // Called only when hasFatalFailure is true, so no jobs will be dispatched.
    // Uses a local queue rather than recursion to avoid stack overflow on deep graphs.
    auto skipNode = [&](uint32_t startIdx)
    {
        std::queue<uint32_t> toSkip;
        toSkip.push(startIdx);
        while (!toSkip.empty()) {
            const auto idx = toSkip.front();
            toSkip.pop();
            auto& nd = impl_->nodes_[idx];
            const auto prev = nd.jobStatus_.exchange(JobStatus::kSkipped, std::memory_order_acq_rel);
            if (prev == JobStatus::kSkipped) continue; // already skipped — avoid re-visiting
            const auto done     = impl_->completedCount_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
            const auto progress = static_cast<float>(done) / static_cast<float>(total);
            if (observer) observer->onFinish(nd.nameStr_, JobStatus::kSkipped, progress);
            for (auto succIdx : nd.successors_) toSkip.push(succIdx);
        }
    };

    // dispatchJob: execute one node, then propagate to ready successors.
    // Must be std::function (not auto lambda) because it references itself
    // when dispatching successors — recursive lambdas need an explicit type.
    std::function<void(uint32_t)> dispatchJob = [&](uint32_t idx)
    {
        auto& nd = impl_->nodes_[idx];

        if (hasFatalFailure.load(std::memory_order_acquire)) {
            skipNode(idx);
            return;
        }

        nd.jobStatus_.store(JobStatus::kRunning, std::memory_order_release);
        if (observer) observer->onStart(nd.nameStr_);

        auto result = nd.fn_();

        const auto jobStatus = result ? JobStatus::kDone : JobStatus::kFailed;
        nd.jobStatus_.store(jobStatus, std::memory_order_release);

        const auto done     = impl_->completedCount_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        const auto progress = static_cast<float>(done) / static_cast<float>(total);

        if (observer) observer->onFinish(nd.nameStr_, jobStatus, progress);

        if (!result && !nd.isOptional_) {
            // Atomically capture only the first fatal error.
            bool expected = false;
            if (hasFatalFailure.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                std::lock_guard lk{fatalMtx};
                fatalError = result.error();
            }
            // Cascade skip to all successors of this failed node.
            for (auto succIdx : nd.successors_) {
                skipNode(succIdx);
            }
            return;
        }

        // Propagate: decrement unmetDeps_ on successors; dispatch those that reach zero.
        for (auto succIdx : nd.successors_) {
            auto& succ       = impl_->nodes_[succIdx];
            const auto prev  = succ.unmetDeps_.load(std::memory_order_relaxed);
            assert(prev > 0U && "unmetDeps_ underflow — duplicate edge?");
            const auto remaining =
                succ.unmetDeps_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
            if (remaining == 0U) {
                succ.jobStatus_.store(JobStatus::kReady, std::memory_order_release);
                executor.dispatch(
                    succ.nameStr_,
                    [&, si = succIdx] { dispatchJob(si); },
                    [] {},
                    succ.coreAffinity_,
                    succ.priority_,
                    succ.stackBytes_);
            }
        }
    };

    // Seed: dispatch all root nodes (no predecessors).
    for (uint32_t i = 0U; i < static_cast<uint32_t>(total); ++i) {
        if (impl_->nodes_[i].jobStatus_.load(std::memory_order_relaxed) == JobStatus::kReady) {
            auto& nd = impl_->nodes_[i];
            executor.dispatch(
                nd.nameStr_,
                [&, idx = i] { dispatchJob(idx); },
                [] {},
                nd.coreAffinity_,
                nd.priority_,
                nd.stackBytes_);
        }
    }

    executor.wait_all();

    if (hasFatalFailure.load(std::memory_order_acquire)) {
        std::lock_guard lk{fatalMtx};
        return std::unexpected(fatalError);
    }
    return {};
}

// ── Status queries ────────────────────────────────────────────────────────────

auto Pipeline::status(Job j) const noexcept -> JobStatus
{
    if (!impl_ || !j.valid() || j.idx_ >= impl_->nodes_.size())
        return JobStatus::kPending;
    return impl_->nodes_[j.idx_].jobStatus_.load(std::memory_order_acquire);
}

auto Pipeline::name(Job j) const noexcept -> std::string_view
{
    if (!impl_ || !j.valid() || j.idx_ >= impl_->nodes_.size())
        return {};
    return impl_->nodes_[j.idx_].nameStr_;
}

// ── Tick loop ─────────────────────────────────────────────────────────────────

void Pipeline::add_tick(TickJob tick)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->ticks_.push_back(std::move(tick));
}

void Pipeline::run_loop()
{
    if (!impl_) {
        // Post-move pipeline — yield continuously rather than busy-spinning.
        while (true) {
#if __has_include(<freertos/FreeRTOS.h>)
            vTaskDelay(1);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
#endif
        }
    }

    struct TickState { std::chrono::steady_clock::time_point lastRun_{}; };
    std::vector<TickState> tickStates(impl_->ticks_.size());

    while (true) {
        const auto now = std::chrono::steady_clock::now();

        for (std::size_t i = 0U; i < impl_->ticks_.size(); ++i) {
            auto& tick  = impl_->ticks_[i];
            auto& state = tickStates[i];
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - state.lastRun_);
            if (elapsed >= tick.interval) {
                tick.fn();
                state.lastRun_ = now;
            }
        }

#if __has_include(<freertos/FreeRTOS.h>)
        vTaskDelay(1);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
#endif
    }
}

// ── On-demand jobs ────────────────────────────────────────────────────────────

Job Pipeline::add_on_demand(std::function<std::expected<void, PipelineError>()> fn)
{
    // TODO: on-demand jobs are registered in the DAG but not yet connected to
    // the event-dispatch mechanism. trigger() is a stub. Do NOT call run() on
    // a Pipeline that has on-demand jobs registered — they will execute
    // immediately as root nodes during the normal boot phase.
    return emplace(std::move(fn));
}

void Pipeline::trigger(Job /*j*/)
{
    // TODO: implement event-triggered execution post-boot.
}

// ── Diagnostics ───────────────────────────────────────────────────────────────

void Pipeline::dump_text() const
{
    if (!impl_) return;
    std::printf("Pipeline DAG (%zu jobs):\n", impl_->nodes_.size());
    for (uint32_t i = 0U; i < static_cast<uint32_t>(impl_->nodes_.size()); ++i) {
        const auto& nd = impl_->nodes_[i];
        std::printf("  [%u] %s (deps:", i, nd.nameStr_.c_str());
        for (auto p : nd.predecessors_)
            std::printf(" %s", impl_->nodes_[p].nameStr_.c_str());
        std::printf(") -> (");
        for (auto s : nd.successors_)
            std::printf(" %s", impl_->nodes_[s].nameStr_.c_str());
        std::printf(")\n");
    }
}

void Pipeline::dump_trace() const
{
    // TODO: emit trace events representing the DAG structure + execution timeline.
}

} // namespace sub0pipeline
