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

// ── PoolSuccessors: pool/arena-backed successor list ──────────────────────────
// 12 bytes: stores up to 4 successor indices inline using uint16_t (node indices
// ≤ 65535 cover all realistic pipelines). Overflow spills into a flat uint16_t
// arena owned by Pipeline::Impl -- zero heap allocation after initial reserve.
//
// Layout (12 bytes, align 2):
//   [0..1]  size_    -- bit[15] = pool-mode flag; bits[14:0] = count (max 32767)
//   [2..3]  poolIdx_ -- uint16_t start offset in Impl::succPool_ (pool mode)
//   [4..11] inline_[4] -- 4 × uint16_t inline entries (8 bytes)
//
// Rationale for uint16_t size_:
//   The previous uint8_t layout used bit[7] as the pool flag, limiting count to
//   127 before silent bit-collision. uint16_t raises that to 32767 -- unreachable
//   in any real pipeline -- without changing the 12-byte struct size.

struct PoolSuccessors
{
    static constexpr uint16_t kInlineCap = 4U;
    static constexpr uint16_t kPoolFlag  = 0x8000U; ///< bit[15] = pool mode
    static constexpr uint16_t kCountMask = 0x7FFFU; ///< bits[14:0] = count

    uint16_t size_{0};      ///< bit[15] = pool flag; bits[14:0] = entry count
    uint16_t poolIdx_{0};   ///< start index into Impl::succPool_ (pool mode only)
    uint16_t inline_[kInlineCap]{};  ///< inline storage: 4 × uint16_t = 8 bytes

    PoolSuccessors()  = default;
    ~PoolSuccessors() = default; // no owned resources -- pool owned by Impl

    PoolSuccessors(PoolSuccessors&& o) noexcept
        : size_{o.size_}, poolIdx_{o.poolIdx_}
    {
        for (uint16_t i = 0; i < kInlineCap; ++i) inline_[i] = o.inline_[i];
        o.size_    = 0;
        o.poolIdx_ = 0;
    }
    PoolSuccessors& operator=(PoolSuccessors&&)      = delete;
    PoolSuccessors(const PoolSuccessors&)             = delete;
    PoolSuccessors& operator=(const PoolSuccessors&) = delete;

    [[nodiscard]] uint16_t count()  const noexcept { return size_ & kCountMask; }
    [[nodiscard]] bool     empty()  const noexcept { return count() == 0; }
    [[nodiscard]] uint16_t size()   const noexcept { return count(); }
    [[nodiscard]] bool     isPool() const noexcept { return (size_ & kPoolFlag) != 0U; }

    // Push a node index into the arena-backed successor list.
    // Pool overflow appends a new contiguous block; the old block is abandoned
    // (wasted but bounded: typically ≤ 35 abandoned entries per node worst case).
    void push_back(uint16_t v, std::vector<uint16_t>& pool)
    {
        const uint16_t n = count();
        if (!isPool() && n < kInlineCap) {
            inline_[n] = v;
            size_ = static_cast<uint16_t>(n + 1U);
        } else if (!isPool()) {
            // Spill inline → pool: append a new contiguous block.
            if (pool.size() >= 0xFFFFU)
                SUB0PIPELINE_THROW("succPool_ exceeded 65535-entry uint16_t address range");
            const auto start = static_cast<uint16_t>(pool.size());
            for (uint16_t i = 0; i < n; ++i) pool.push_back(inline_[i]);
            pool.push_back(v);
            poolIdx_ = start;
            size_    = static_cast<uint16_t>(kPoolFlag | (n + 1U));
        } else {
            // Grow pool: allocate a fresh contiguous block (old block orphaned).
            if (pool.size() >= 0xFFFFU)
                SUB0PIPELINE_THROW("succPool_ exceeded 65535-entry uint16_t address range");
            const auto start = static_cast<uint16_t>(pool.size());
            for (uint16_t i = 0; i < n; ++i) pool.push_back(pool[poolIdx_ + i]);
            pool.push_back(v);
            poolIdx_ = start;
            size_    = static_cast<uint16_t>(kPoolFlag | (n + 1U));
        }
    }

    // Returns a pointer into either inline_ or Impl::succPool_.data() + poolIdx_.
    // Pool pointer is valid until Impl::succPool_ is next modified (reallocated).
    const uint16_t* begin(const std::vector<uint16_t>& pool) const noexcept
    { return isPool() ? pool.data() + poolIdx_ : inline_; }
    const uint16_t* end(const std::vector<uint16_t>& pool)   const noexcept
    { return begin(pool) + count(); }

    // Range adapter for range-based for: `for (auto s : nd.successors_.range(pool))`
    struct Range {
        const uint16_t* b_; const uint16_t* e_;
        const uint16_t* begin() const noexcept { return b_; }
        const uint16_t* end()   const noexcept { return e_; }
    };
    Range range(const std::vector<uint16_t>& pool) const noexcept
    { return {begin(pool), end(pool)}; }
};
static_assert(sizeof(PoolSuccessors) == 12,
    "PoolSuccessors must be exactly 12 bytes");

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
    uint8_t                 _pad0[1]{};         ///< 1 byte pad: brings offset to 8, aligning lastEpoch_ to 4.
    uint32_t                lastEpoch_{0U};
    int                     coreAffinity_{-1};
    uint32_t                stackBytes_{8192U};
    uint32_t                predecessorCount_{0U}; ///< Replaces predecessors_.size() in resetNode.
    std::chrono::milliseconds timeout_{std::chrono::milliseconds::max()};
    PoolSuccessors          successors_;            ///< Iterated on completion -- kept hot (12 B, inline ≤4, pool up to 32767).

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
        , successors_{std::move(o.successors_)}   // PoolSuccessors move ctor
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
    std::vector<uint16_t>     succPool_;    ///< Flat arena for overflow successor indices (see PoolSuccessors).
    uint32_t                  runEpoch_{0U};
    bool                      rootsCached_{false};
    IExecutor*                armedExecutor_{nullptr};
    IObserver*                armedObserver_{nullptr};

    // Written once (under fatalMtx) when the first non-optional job fails.
    // Readable after run() returns via Pipeline::first_failure_name().
    std::string               failedJobName_;

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
    otherNode.successors_.push_back(static_cast<uint16_t>(idx_), pipeline_->impl_->succPool_);
    return *this;
}

Job& Job::precede(Job other)
{
    if (!pipeline_ || !valid() || !other.valid()) return *this;
    auto& selfNode  = pipeline_->node(idx_);
    auto& otherNode = pipeline_->node(other.idx_);
    selfNode.successors_.push_back(static_cast<uint16_t>(other.idx_), pipeline_->impl_->succPool_);
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
    impl_->rootsCached_ = false; // new node may be a root; invalidate cached set
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
    impl_->rootsCached_ = false;
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
    impl_->rootsCached_ = false;
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
        for (auto succ : nd.successors_.range(impl_->succPool_))
            ++inDegree[succ];
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
        for (auto succ : impl_->nodes_[idx].successors_.range(impl_->succPool_)) {
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
    // Previous: 272B (original) → 176B (first pass) → 168B (pool/arena pass).
    // Raise only for an intentional new field; do not raise to paper over bloat.
    static_assert(sizeof(Node) <= 168,
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
    impl_->failedJobName_.clear();

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
                for (auto succIdx : nd.successors_.range(impl.succPool_)) toSkip.push(succIdx);
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
                    impl.failedJobName_ = nd.nameStr_;
                }
                for (auto succIdx : nd.successors_.range(impl.succPool_)) skipNode(succIdx);
                return;
            }

            for (auto succIdx : nd.successors_.range(impl.succPool_)) {
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

std::string_view Pipeline::first_failure_name() const noexcept
{
    if (!impl_) return {};
    return impl_->failedJobName_;
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

void Pipeline::run_until(IExecutor& executor, std::stop_token stop,
                         IObserver* observer,
                         std::function<void(PipelineError)> onError)
{
    while (!stop.stop_requested()) {
        auto result = run(executor, observer);
        if (!result && onError)
            onError(result.error());
    }
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

    // Capture by index, not by nd reference: nodes_ may reallocate if another
    // add_on_demand() is called before the dispatch lambda executes.
    exec->dispatch(
        nd.nameStr_,
        [impl = impl_.get(), idx = j.idx_, obs]
        {
            auto& n = impl->nodes_[idx];
            n.jobStatus_.store(JobStatus::kRunning, std::memory_order_release);
            if (obs) obs->onStart(n.nameStr_);

            auto result = n.fn_(n.stopSource_.get_token());

            const auto status = result ? JobStatus::kDone : JobStatus::kFailed;
            n.jobStatus_.store(status, std::memory_order_release);
            if (obs) obs->onFinish(n.nameStr_, status, 1.0f);
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
        for (auto s : nd.successors_.range(impl_->succPool_))
            std::printf(" %s", impl_->nodes_[s].nameStr_.c_str());
        std::printf(")\n");
    }
}

void Pipeline::dump_trace() const
{
    // TODO: emit trace events representing the DAG structure + execution timeline.
}

} // namespace sub0pipeline
