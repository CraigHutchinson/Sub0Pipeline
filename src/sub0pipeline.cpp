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
#include <future>
#include <mutex>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
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

// ── InlineSuccessors: small-buffer successor list ─────────────────────────────
// Exactly 16 bytes: stores up to 2 successor indices inline (covers ~80% of
// real-world nodes), spilling to heap for 3+. Replaces std::vector<uint32_t>
// (24 bytes, always heap-allocates).
//
// Layout (16 bytes, 8-byte aligned):
//   [0]     size_ -- bits[6:0] = count, bit[7] = heap mode
//   [1..7]  _pad7 -- explicit pad to align union to offset 8
//   [8..15] union: inline_[2] (2×uint32_t) OR heap_ (uint32_t*)
//
// Heap grows by exactly 1 per push beyond kInlineCap; allocation count is
// bounded by node out-degree which is typically ≤5 in practice.

struct InlineSuccessors
{
    static constexpr uint8_t kInlineCap = 2U;
    static constexpr uint8_t kHeapFlag  = 0x80U;

    uint8_t  size_{0};    ///< bits[6:0] = count; bit[7] = heap-mode flag
    uint8_t  _pad7[7]{};  ///< explicit: places union at offset 8 (8-byte aligned)
    union {
        uint32_t  inline_[kInlineCap]; ///< inline storage (2 × uint32_t = 8 bytes)
        uint32_t* heap_{nullptr};      ///< heap pointer when kHeapFlag is set
    };

    InlineSuccessors()  = default;
    ~InlineSuccessors() { if (size_ & kHeapFlag) delete[] heap_; }

    InlineSuccessors(InlineSuccessors&& o) noexcept : size_{o.size_}
    {
        if (size_ & kHeapFlag) {
            heap_ = o.heap_; o.heap_ = nullptr; o.size_ = 0;
        } else {
            const auto n = count();
            for (uint8_t i = 0; i < n; ++i) inline_[i] = o.inline_[i];
        }
    }
    InlineSuccessors& operator=(InlineSuccessors&&)      = delete;
    InlineSuccessors(const InlineSuccessors&)             = delete;
    InlineSuccessors& operator=(const InlineSuccessors&) = delete;

    [[nodiscard]] uint8_t count()  const noexcept { return size_ & 0x7FU; }
    [[nodiscard]] bool    empty()  const noexcept { return count() == 0; }
    [[nodiscard]] uint8_t size()   const noexcept { return count(); }
    [[nodiscard]] bool    isHeap() const noexcept { return (size_ & kHeapFlag) != 0U; }

    void push_back(uint32_t v)
    {
        const uint8_t n = count();
        if (!isHeap() && n < kInlineCap) {
            inline_[n] = v;
            size_ = static_cast<uint8_t>(n + 1U);
        } else if (!isHeap()) {
            // Spill inline → heap: allocate exact size
            uint32_t* p = new uint32_t[n + 1U];
            for (uint8_t i = 0; i < n; ++i) p[i] = inline_[i];
            p[n] = v;
            heap_  = p;
            size_  = static_cast<uint8_t>(kHeapFlag | (n + 1U));
        } else {
            // Grow heap by 1 (out-degree is small, O(n²) is negligible here)
            uint32_t* p = new uint32_t[n + 1U];
            for (uint8_t i = 0; i < n; ++i) p[i] = heap_[i];
            p[n] = v;
            delete[] heap_;
            heap_ = p;
            size_ = static_cast<uint8_t>(kHeapFlag | (n + 1U));
        }
    }

    const uint32_t* begin() const noexcept { return isHeap() ? heap_ : inline_; }
    const uint32_t* end()   const noexcept { return begin() + count(); }
    uint32_t*       begin() noexcept       { return isHeap() ? heap_ : inline_; }
    uint32_t*       end()   noexcept       { return begin() + count(); }
};
static_assert(sizeof(InlineSuccessors) == 16,
    "InlineSuccessors must be exactly 16 bytes");

// ── Internal node ─────────────────────────────────────────────────────────────

struct Pipeline::Node
{
    // ── Hot section: touched every dispatch cycle ─────────────────────────
    // Placed first so the hot fields and successors_ sit in the fewest cache
    // lines possible when dispatchJob accesses them from concurrent threads.

    std::atomic<uint32_t>   unmetDeps_{0U};    ///< Decremented by completing predecessors.
    std::atomic<JobStatus>  jobStatus_{JobStatus::kPending};
    uint8_t                 flags_{0U};         ///< Packed bool flags -- see kFlag* constants.
    uint8_t                 priority_{5U};
    uint8_t                 _pad0[2]{};         ///< Explicit pad: aligns lastEpoch_ to 4.
    uint32_t                lastEpoch_{0U};
    int                     coreAffinity_{-1};
    uint32_t                stackBytes_{8192U};
    uint32_t                predecessorCount_{0U}; ///< Replaces predecessors_.size() in resetNode.
    std::chrono::milliseconds timeout_{std::chrono::milliseconds::max()};
    InlineSuccessors        successors_;            ///< Iterated on completion -- kept hot (16 B, inline up to 3).

    // ── Dispatch section: read once at the start of each job execution ────
    std::function<std::expected<void, PipelineError>(std::stop_token)> fn_;
    std::stop_source        stopSource_;

    // ── Cold section: build-time only ────────────────────────────────────
    std::string             nameStr_;
    const char*             statusText_{nullptr};

    // ── Flag constants ────────────────────────────────────────────────────
    static constexpr uint8_t kFlagOptional    = 0x01; ///< Failure does not block dependents.
    static constexpr uint8_t kFlagOnDemand    = 0x02; ///< Excluded from run() root set.
    static constexpr uint8_t kFlagCancellable = 0x04; ///< fn_ checks stop_token cooperatively.

    bool isOptional()    const noexcept { return (flags_ & kFlagOptional)    != 0U; }
    bool isOnDemand()    const noexcept { return (flags_ & kFlagOnDemand)    != 0U; }
    bool isCancellable() const noexcept { return (flags_ & kFlagCancellable) != 0U; }

    Node() = default;

    // std::atomic is non-copyable/non-movable, but std::vector may reallocate
    // nodes during the build phase. Loading in the move constructor is safe
    // because nodes are only moved before execution begins.
    Node(Node&& o) noexcept
        : unmetDeps_{o.unmetDeps_.load(std::memory_order_relaxed)}
        , jobStatus_{o.jobStatus_.load(std::memory_order_relaxed)}
        , flags_{o.flags_}
        , priority_{o.priority_}
        , lastEpoch_{o.lastEpoch_}
        , coreAffinity_{o.coreAffinity_}
        , stackBytes_{o.stackBytes_}
        , predecessorCount_{o.predecessorCount_}
        , timeout_{o.timeout_}
        , successors_{std::move(o.successors_)}   // InlineSuccessors move ctor
        , fn_{std::move(o.fn_)}
        , stopSource_{std::move(o.stopSource_)}
        , nameStr_{std::move(o.nameStr_)}
        , statusText_{o.statusText_}
    {}

    Node& operator=(Node&&)      = delete;
    Node(const Node&)            = delete;
    Node& operator=(const Node&) = delete;
};

// ── Pipeline implementation structure ─────────────────────────────────────────

struct Pipeline::Impl
{
    // Build-time / init-time fields (written once, read during run setup).
    std::vector<Node>         nodes_;
    std::vector<TickJob>      ticks_;
    std::vector<uint32_t>     roots_;
    uint32_t                  runEpoch_{0U};
    bool                      rootsCached_{false};
    IExecutor*                armedExecutor_{nullptr};
    IObserver*                armedObserver_{nullptr};

    // Hot atomic: written by every completing job thread on every run.
    // alignas(64) isolates it from the fields above so concurrent job-thread
    // writes to completedCount_ do not cause false sharing with runEpoch_ or
    // rootsCached_ which the run-owning thread reads/writes.
    alignas(64) std::atomic<uint32_t> completedCount_{0U};
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
    if (pipeline_ && valid()) {
        auto& nd = pipeline_->node(idx_);
        if (opt) nd.flags_ |= Pipeline::Node::kFlagOptional;
        else     nd.flags_ &= static_cast<uint8_t>(~Pipeline::Node::kFlagOptional);
    }
    return *this;
}

Job& Job::status(const char* text) noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).statusText_ = text;
    return *this;
}

void Job::cancel() noexcept
{
    if (pipeline_ && valid()) pipeline_->node(idx_).stopSource_.request_stop();
}

Job& Job::succeed(Job other)
{
    if (!pipeline_ || !valid() || !other.valid()) return *this;
    auto& selfNode  = pipeline_->node(idx_);
    auto& otherNode = pipeline_->node(other.idx_);
    ++selfNode.predecessorCount_;
    otherNode.successors_.push_back(idx_);
    return *this;
}

Job& Job::precede(Job other)
{
    if (!pipeline_ || !valid() || !other.valid()) return *this;
    auto& selfNode  = pipeline_->node(idx_);
    auto& otherNode = pipeline_->node(other.idx_);
    selfNode.successors_.push_back(other.idx_);
    ++otherNode.predecessorCount_;
    return *this;
}

// ── Job dependency on JobGroup ────────────────────────────────────────────────

Job& Job::succeed(JobGroup const& group)
{
    for (auto j : group.jobs()) succeed(j);
    return *this;
}

Job& Job::precede(JobGroup const& group)
{
    for (auto j : group.jobs()) precede(j);
    return *this;
}

// ── JobGroup methods ─────────────────────────────────────────────────────────

JobGroup& JobGroup::succeed(Job other)
{
    for (auto& j : jobs_) j.succeed(other);
    return *this;
}

JobGroup& JobGroup::succeed(JobGroup const& other)
{
    for (auto& j : jobs_)
        for (auto o : other.jobs()) j.succeed(o);
    return *this;
}

JobGroup& JobGroup::precede(Job other)
{
    for (auto& j : jobs_) j.precede(other);
    return *this;
}

JobGroup& JobGroup::precede(JobGroup const& other)
{
    for (auto& j : jobs_)
        for (auto o : other.jobs()) j.precede(o);
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
    auto& nd    = impl_->nodes_.back();
    // Wrap non-cancellable fn in the unified (stop_token) signature.
    // kFlagCancellable is NOT set -- dispatchJob uses hard packaged_task cutoff
    // for timeout enforcement since the wrapped fn ignores the token.
    nd.fn_      = [f = std::move(fn)](std::stop_token) -> std::expected<void, PipelineError>
                  { return f(); };
    nd.nameStr_ = "job_" + std::to_string(idx);
    return Job{idx, this};
}

Job Pipeline::emplace(
    std::function<std::expected<void, PipelineError>(std::stop_token)> fn)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    const auto idx = static_cast<uint32_t>(impl_->nodes_.size());
    impl_->nodes_.emplace_back();
    auto& nd           = impl_->nodes_.back();
    nd.fn_             = std::move(fn);
    nd.flags_         |= Node::kFlagCancellable;
    nd.nameStr_        = "job_" + std::to_string(idx);
    return Job{idx, this};
}

Job Pipeline::emplace_void(std::function<void()> fn)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    const auto idx = static_cast<uint32_t>(impl_->nodes_.size());
    impl_->nodes_.emplace_back();
    auto& nd    = impl_->nodes_.back();
    nd.fn_      = [f = std::move(fn)](std::stop_token) -> std::expected<void, PipelineError>
                  { f(); return {}; };
    nd.nameStr_ = "job_" + std::to_string(idx);
    return Job{idx, this};
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
    static_assert(sizeof(Node) <= 184,
        "Pipeline::Node exceeded size budget -- check padding or new large fields");

    if (!impl_ || impl_->nodes_.empty()) return {};
    if (auto v = validate(); !v) return v;

    const auto total = impl_->nodes_.size();
    impl_->completedCount_.store(0U, std::memory_order_relaxed);

    if (!impl_->rootsCached_) {
        impl_->roots_.clear();
        for (uint32_t i = 0U; i < static_cast<uint32_t>(total); ++i) {
            const auto& nd = impl_->nodes_[i];
            if (nd.predecessorCount_ == 0U && !nd.isOnDemand())
                impl_->roots_.push_back(i);
        }
        impl_->rootsCached_ = true;
    }

    const auto epoch = ++impl_->runEpoch_;

    std::atomic<bool> hasFatalFailure{false};
    PipelineError     fatalError{PipelineError::kJobFailed};
    std::mutex        fatalMtx;

    // DispatchContext: replaces the previous std::function<void(uint32_t)>
    // self-referential lambda. Defined as a local struct inside run() so it
    // has access to Pipeline's private Impl and Node types.
    //
    // executor.dispatch() receives [&ctx, si] (pointer + uint32_t = 12 bytes)
    // which fits in std::function SSO, eliminating one heap alloc per dispatch.
    struct DispatchContext
    {
        Impl&               impl;
        IExecutor&          executor;
        IObserver*          observer;
        uint32_t            total;
        uint32_t            epoch;
        std::atomic<bool>&  hasFatalFailure;
        PipelineError&      fatalError;
        std::mutex&         fatalMtx;

        void resetNode(Node& nd) const noexcept
        {
            if (nd.lastEpoch_ == epoch) return;
            nd.lastEpoch_ = epoch;
            nd.stopSource_ = std::stop_source{};
            nd.unmetDeps_.store(nd.predecessorCount_, std::memory_order_relaxed);
            nd.jobStatus_.store(
                nd.predecessorCount_ == 0U ? JobStatus::kReady : JobStatus::kPending,
                std::memory_order_relaxed);
        }

        void skipNode(uint32_t startIdx)
        {
            std::queue<uint32_t> toSkip;
            toSkip.push(startIdx);
            while (!toSkip.empty()) {
                const auto idx = toSkip.front(); toSkip.pop();
                auto& nd = impl.nodes_[idx];
                resetNode(nd);
                const auto prev = nd.jobStatus_.exchange(JobStatus::kSkipped, std::memory_order_acq_rel);
                if (prev == JobStatus::kSkipped) continue;
                const auto done     = impl.completedCount_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
                const auto progress = static_cast<float>(done) / static_cast<float>(total);
                if (observer) observer->onFinish(nd.nameStr_, JobStatus::kSkipped, progress);
                for (auto succIdx : nd.successors_) toSkip.push(succIdx);
            }
        }

        void dispatchJob(uint32_t idx)
        {
            auto& nd = impl.nodes_[idx];
            if (hasFatalFailure.load(std::memory_order_acquire)) { skipNode(idx); return; }

            nd.jobStatus_.store(JobStatus::kRunning, std::memory_order_release);
            if (observer) observer->onStart(nd.nameStr_);

            std::expected<void, PipelineError> result;
            auto token = nd.stopSource_.get_token();
            const bool hasTimeout = nd.timeout_ < std::chrono::milliseconds::max();

            if (nd.isCancellable() && hasTimeout) {
                std::jthread watchdog{[src = nd.stopSource_, dur = nd.timeout_]
                    (std::stop_token wdStop) mutable {
                    std::this_thread::sleep_for(dur);
                    if (!wdStop.stop_requested()) src.request_stop();
                }};
                result = nd.fn_(token);
            } else if (!nd.isCancellable() && hasTimeout) {
                auto fnCopy = nd.fn_;
                std::packaged_task<std::expected<void, PipelineError>()> task{
                    [f = std::move(fnCopy), tok = token]{ return f(tok); }
                };
                auto fut = task.get_future();
                std::thread jobThread{std::move(task)};
                if (fut.wait_for(nd.timeout_) == std::future_status::timeout) {
                    nd.stopSource_.request_stop();
                    jobThread.detach();
                    result = std::unexpected(PipelineError::kTimeout);
                } else {
                    jobThread.join();
                    result = fut.get();
                }
            } else {
                result = nd.fn_(token);
            }

            const auto jobStatus = [&result]() -> JobStatus {
                if (result) return JobStatus::kDone;
                switch (result.error()) {
                    case PipelineError::kTimeout:   return JobStatus::kTimedOut;
                    case PipelineError::kCancelled: return JobStatus::kCancelled;
                    default:                        return JobStatus::kFailed;
                }
            }();
            nd.jobStatus_.store(jobStatus, std::memory_order_release);

            const auto done     = impl.completedCount_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
            const auto progress = static_cast<float>(done) / static_cast<float>(total);
            if (observer) observer->onFinish(nd.nameStr_, jobStatus, progress);

            if (!result && !nd.isOptional()) {
                bool expected = false;
                if (hasFatalFailure.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    std::lock_guard lk{fatalMtx};
                    fatalError = result.error();
                }
                for (auto succIdx : nd.successors_) skipNode(succIdx);
                return;
            }

            for (auto succIdx : nd.successors_) {
                auto& succ = impl.nodes_[succIdx];
                resetNode(succ);
                const auto remaining = succ.unmetDeps_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
                if (remaining == 0U) {
                    succ.jobStatus_.store(JobStatus::kReady, std::memory_order_release);
                    executor.dispatch(
                        succ.nameStr_,
                        [this, si = succIdx]{ dispatchJob(si); },
                        [] {},
                        succ.coreAffinity_, succ.priority_, succ.stackBytes_);
                }
            }
        }
    };

    DispatchContext ctx{*impl_, executor, observer,
                        static_cast<uint32_t>(total), epoch,
                        hasFatalFailure, fatalError, fatalMtx};

    for (auto idx : impl_->roots_) {
        auto& nd = impl_->nodes_[idx];
        ctx.resetNode(nd);
        executor.dispatch(
            nd.nameStr_,
            [&ctx, i = idx]{ ctx.dispatchJob(i); },
            [] {},
            nd.coreAffinity_, nd.priority_, nd.stackBytes_);
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

auto Pipeline::run_inline(IObserver* observer) -> std::expected<void, PipelineError>
{
    struct InlineExecutor final : IExecutor
    {
        void dispatch(std::string_view, std::function<void()> fn,
                      std::function<void()> oc, int, uint8_t, uint32_t) override
        { fn(); if (oc) oc(); }
        void wait_all() override {}
        [[nodiscard]] int concurrency() const noexcept override { return 1; }
    } exec;
    return run(exec, observer);
}

void Pipeline::run_until(IExecutor& executor, std::stop_token stop)
{
    while (!stop.stop_requested())
        (void)run(executor);
}

// ── On-demand jobs ────────────────────────────────────────────────────────────

void Pipeline::arm(IExecutor& executor, IObserver* observer) noexcept
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->armedExecutor_ = &executor;
    impl_->armedObserver_ = observer;
}

Job Pipeline::add_on_demand(std::function<std::expected<void, PipelineError>()> fn)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    const auto idx = static_cast<uint32_t>(impl_->nodes_.size());
    impl_->nodes_.emplace_back();
    auto& nd       = impl_->nodes_.back();
    nd.fn_     = [f = std::move(fn)](std::stop_token) -> std::expected<void, PipelineError>
               { return f(); };
    nd.nameStr_ = "on_demand_" + std::to_string(idx);
    nd.flags_  |= Pipeline::Node::kFlagOnDemand;
    // Invalidate root cache -- the new on-demand node would otherwise be
    // picked up as a root on the next run() (it has no predecessors).
    impl_->rootsCached_ = false;
    return Job{idx, this};
}

Job Pipeline::add_on_demand(
    std::function<std::expected<void, PipelineError>(std::stop_token)> fn)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    const auto idx = static_cast<uint32_t>(impl_->nodes_.size());
    impl_->nodes_.emplace_back();
    auto& nd    = impl_->nodes_.back();
    nd.fn_      = std::move(fn);
    nd.nameStr_ = "on_demand_" + std::to_string(idx);
    nd.flags_  |= Pipeline::Node::kFlagOnDemand | Pipeline::Node::kFlagCancellable;
    impl_->rootsCached_ = false;
    return Job{idx, this};
}

auto Pipeline::trigger(Job j) -> std::expected<void, PipelineError>
{
    if (!impl_ || !j.valid() || j.idx_ >= impl_->nodes_.size())
        return std::unexpected(PipelineError::kUnknownJob);

    if (!impl_->armedExecutor_)
        return std::unexpected(PipelineError::kNotArmed);

    auto& nd = impl_->nodes_[j.idx_];
    if (!nd.isOnDemand())
        return std::unexpected(PipelineError::kNotOnDemand);

    IExecutor*  exec = impl_->armedExecutor_;
    IObserver*  obs  = impl_->armedObserver_;

    nd.jobStatus_.store(JobStatus::kReady, std::memory_order_release);

    exec->dispatch(
        nd.nameStr_,
        [&nd, obs]
        {
            nd.jobStatus_.store(JobStatus::kRunning, std::memory_order_release);
            if (obs) obs->onStart(nd.nameStr_);

            auto result = nd.fn_(nd.stopSource_.get_token());

            const auto status = result ? JobStatus::kDone : JobStatus::kFailed;
            nd.jobStatus_.store(status, std::memory_order_release);
            if (obs) obs->onFinish(nd.nameStr_, status, 1.0f);
        },
        [] {},
        nd.coreAffinity_,
        nd.priority_,
        nd.stackBytes_);

    return {};
}

// ── Diagnostics ───────────────────────────────────────────────────────────────

void Pipeline::dump_text() const
{
    if (!impl_) return;
    std::printf("Pipeline DAG (%zu jobs):\n", impl_->nodes_.size());
    for (uint32_t i = 0U; i < static_cast<uint32_t>(impl_->nodes_.size()); ++i) {
        const auto& nd = impl_->nodes_[i];
        std::printf("  [%u] %s (predecessors: %u) -> (", i, nd.nameStr_.c_str(), nd.predecessorCount_);
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
