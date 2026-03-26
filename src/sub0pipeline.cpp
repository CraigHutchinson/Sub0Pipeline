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
    std::string                                          name_str;
    std::function<std::expected<void, PipelineError>()> fn;
    std::chrono::milliseconds                            timeout{30'000};
    int                                                  core_affinity{-1};
    uint8_t                                              priority{5U};
    uint32_t                                             stack_bytes{8192U};
    bool                                                 is_optional{false};
    const char*                                          status_text{nullptr};

    std::vector<uint32_t>   predecessors;
    std::vector<uint32_t>   successors;

    // std::atomic is non-copyable/non-movable by default, but std::vector may
    // reallocate nodes during the build phase. Load in the move constructor is
    // safe because nodes are only moved before execution begins.
    std::atomic<JobStatus>  job_status{JobStatus::kPending};
    std::atomic<uint32_t>   unmet_deps{0U};

    Node() = default;

    Node(Node&& o) noexcept
        : name_str{std::move(o.name_str)}
        , fn{std::move(o.fn)}
        , timeout{o.timeout}
        , core_affinity{o.core_affinity}
        , priority{o.priority}
        , stack_bytes{o.stack_bytes}
        , is_optional{o.is_optional}
        , status_text{o.status_text}
        , predecessors{std::move(o.predecessors)}
        , successors{std::move(o.successors)}
        , job_status{o.job_status.load(std::memory_order_relaxed)}
        , unmet_deps{o.unmet_deps.load(std::memory_order_relaxed)}
    {}

    Node& operator=(Node&&)      = delete;
    Node(const Node&)            = delete;
    Node& operator=(const Node&) = delete;
};

// ── Pipeline implementation structure ─────────────────────────────────────────

struct Pipeline::Impl
{
    std::vector<Node>    nodes;
    std::vector<TickJob> ticks;
    uint32_t             completed_count{0U};
};

// ── Job builder methods ───────────────────────────────────────────────────────

Job& Job::name(std::string_view n) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).name_str = std::string{n};
    return *this;
}

Job& Job::timeout(std::chrono::milliseconds t) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).timeout = t;
    return *this;
}

Job& Job::stack(uint32_t bytes) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).stack_bytes = bytes;
    return *this;
}

Job& Job::core(int c) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).core_affinity = c;
    return *this;
}

Job& Job::priority(uint8_t p) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).priority = p;
    return *this;
}

Job& Job::optional(bool opt) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).is_optional = opt;
    return *this;
}

Job& Job::status(const char* text) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).status_text = text;
    return *this;
}

Job& Job::succeed(Job other) noexcept
{
    if (!pipeline_ || !valid() || !other.valid()) return *this;
    auto& selfNode  = pipeline_->node(idx_);
    auto& otherNode = pipeline_->node(other.idx_);
    selfNode.predecessors.push_back(other.idx_);
    otherNode.successors.push_back(idx_);
    return *this;
}

Job& Job::precede(Job other) noexcept
{
    if (!pipeline_ || !valid() || !other.valid()) return *this;
    auto& selfNode  = pipeline_->node(idx_);
    auto& otherNode = pipeline_->node(other.idx_);
    selfNode.successors.push_back(other.idx_);
    otherNode.predecessors.push_back(idx_);
    return *this;
}

// ── Pipeline lifecycle ────────────────────────────────────────────────────────

Pipeline::Pipeline() : impl_{std::make_unique<Impl>()} {}
Pipeline::~Pipeline() = default;

// ── Internal node access ──────────────────────────────────────────────────────

Pipeline::Node& Pipeline::node(uint32_t idx)
{
    return impl_->nodes[idx];
}

const Pipeline::Node& Pipeline::node(uint32_t idx) const
{
    return impl_->nodes[idx];
}

// ── DAG construction ──────────────────────────────────────────────────────────

Job Pipeline::emplace(std::function<std::expected<void, PipelineError>()> fn)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    const auto idx = static_cast<uint32_t>(impl_->nodes.size());
    impl_->nodes.emplace_back();
    impl_->nodes.back().fn       = std::move(fn);
    impl_->nodes.back().name_str = "job_" + std::to_string(idx);
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
    return impl_ ? impl_->nodes.size() : 0U;
}

// ── Validation ────────────────────────────────────────────────────────────────

auto Pipeline::validate() const -> std::expected<void, PipelineError>
{
    if (!impl_) return {};

    const auto n = impl_->nodes.size();

    // Kahn's algorithm: topological sort — cycle detected if not all nodes visited.
    std::vector<uint32_t> inDegree(n, 0U);
    for (const auto& nd : impl_->nodes) {
        for (auto succ : nd.successors) {
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
        for (auto succ : impl_->nodes[idx].successors) {
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
    if (!impl_ || impl_->nodes.empty()) return {};

    if (auto v = validate(); !v) return v;

    const auto total          = impl_->nodes.size();
    impl_->completed_count    = 0U;

    // Initialise per-node runtime state.
    for (auto& nd : impl_->nodes) {
        nd.unmet_deps.store(
            static_cast<uint32_t>(nd.predecessors.size()),
            std::memory_order_relaxed);
        nd.job_status.store(
            nd.predecessors.empty() ? JobStatus::kReady : JobStatus::kPending,
            std::memory_order_relaxed);
    }

    std::atomic<bool> hasFatalFailure{false};
    PipelineError     fatalError{PipelineError::kJobFailed};

    // dispatch_job is a std::function (not auto) so it can reference itself
    // when dispatching successors — recursive lambdas require an explicit type.
    std::function<void(uint32_t)> dispatchJob = [&](uint32_t idx)
    {
        auto& nd = impl_->nodes[idx];

        if (hasFatalFailure.load(std::memory_order_relaxed)) {
            nd.job_status.store(JobStatus::kSkipped, std::memory_order_release);
            return;
        }

        nd.job_status.store(JobStatus::kRunning, std::memory_order_release);
        if (observer) observer->on_start(nd.name_str);

        auto result = nd.fn();

        const auto jobStatus = result ? JobStatus::kDone : JobStatus::kFailed;
        nd.job_status.store(jobStatus, std::memory_order_release);

        const auto done     = ++impl_->completed_count;
        const auto progress = static_cast<float>(done) / static_cast<float>(total);

        if (observer) observer->on_finish(nd.name_str, jobStatus, progress);

        if (!result && !nd.is_optional) {
            hasFatalFailure.store(true, std::memory_order_release);
            fatalError = result.error();
            return;
        }

        // Propagate: decrement unmet_deps on successors and dispatch any that
        // reach zero (all predecessors done).
        for (auto succIdx : nd.successors) {
            auto& succ      = impl_->nodes[succIdx];
            const auto remaining =
                succ.unmet_deps.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
            if (remaining == 0U) {
                succ.job_status.store(JobStatus::kReady, std::memory_order_release);
                executor.dispatch(
                    succ.name_str,
                    [&, si = succIdx] { dispatchJob(si); },
                    [] {},
                    succ.core_affinity,
                    succ.priority,
                    succ.stack_bytes);
            }
        }
    };

    // Seed: dispatch all root nodes (no predecessors).
    for (uint32_t i = 0U; i < static_cast<uint32_t>(total); ++i) {
        if (impl_->nodes[i].job_status.load(std::memory_order_relaxed) == JobStatus::kReady) {
            auto& nd = impl_->nodes[i];
            executor.dispatch(
                nd.name_str,
                [&, idx = i] { dispatchJob(idx); },
                [] {},
                nd.core_affinity,
                nd.priority,
                nd.stack_bytes);
        }
    }

    executor.wait_all();

    if (hasFatalFailure.load(std::memory_order_acquire))
        return std::unexpected(fatalError);

    return {};
}

// ── Status queries ────────────────────────────────────────────────────────────

auto Pipeline::status(Job j) const noexcept -> JobStatus
{
    if (!impl_ || !j.valid() || j.idx_ >= impl_->nodes.size())
        return JobStatus::kPending;
    return impl_->nodes[j.idx_].job_status.load(std::memory_order_acquire);
}

auto Pipeline::name(Job j) const noexcept -> std::string_view
{
    if (!impl_ || !j.valid() || j.idx_ >= impl_->nodes.size())
        return {};
    return impl_->nodes[j.idx_].name_str;
}

// ── Tick loop ─────────────────────────────────────────────────────────────────

void Pipeline::add_tick(TickJob tick)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->ticks.push_back(std::move(tick));
}

void Pipeline::run_loop()
{
    if (!impl_) { while (true) {} }

    struct TickState { std::chrono::steady_clock::time_point lastRun{}; };
    std::vector<TickState> tickStates(impl_->ticks.size());

    while (true) {
        const auto now = std::chrono::steady_clock::now();

        for (std::size_t i = 0U; i < impl_->ticks.size(); ++i) {
            auto& tick  = impl_->ticks[i];
            auto& state = tickStates[i];
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - state.lastRun);
            if (elapsed >= tick.interval) {
                tick.fn();
                state.lastRun = now;
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
    std::printf("Pipeline DAG (%zu jobs):\n", impl_->nodes.size());
    for (uint32_t i = 0U; i < static_cast<uint32_t>(impl_->nodes.size()); ++i) {
        const auto& nd = impl_->nodes[i];
        std::printf("  [%u] %s (deps:", i, nd.name_str.c_str());
        for (auto p : nd.predecessors) std::printf(" %s", impl_->nodes[p].name_str.c_str());
        std::printf(") -> (");
        for (auto s : nd.successors) std::printf(" %s", impl_->nodes[s].name_str.c_str());
        std::printf(")\n");
    }
}

void Pipeline::dump_trace() const
{
    // TODO: emit trace events representing the DAG structure + execution timeline.
}

} // namespace sub0pipeline
