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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sub0pipeline/deadline.hpp"
#include "sub0pipeline/sub0pipeline.hpp"

// Forces inlining where the compiler's own heuristic declines and the cost was measured.
#if defined(_MSC_VER)
#define SUB0PIPELINE_FORCEINLINE [[msvc::forceinline]]
#elif defined(__GNUC__) || defined(__clang__)
#define SUB0PIPELINE_FORCEINLINE [[gnu::always_inline]] inline
#else
#define SUB0PIPELINE_FORCEINLINE inline
#endif

namespace sub0pipeline
{

namespace
{
// Jobs call Pipeline::setCurrentJobError() on the failure branch only.
// The string is read once (inside dispatchJob, after the job fn returns) and
// immediately cleared.  On the success path the string is never touched --
// zero allocation, zero reads, branch-predictor-friendly empty check.
thread_local std::string t_jobError;

// No executor dispatch is in progress in the finishing job.
constexpr uint32_t kNoSubmission = std::numeric_limits<uint32_t>::max();

JobStatus statusOf(const std::expected<void, PipelineError>& result) noexcept
{
    if (result) return JobStatus::kDone;
    switch (result.error())
    {
        case PipelineError::kCancelled: return JobStatus::kCancelled;
        case PipelineError::kTimeout: return JobStatus::kTimedOut;
        default: return JobStatus::kFailed;
    }
}

void notifyDependencies(IObserver& observer, Pipeline& pipeline, RunId runId,
                       JobId from, std::string_view fromName)
{
    auto successors = pipeline.successors(from);
    if (!successors.empty())
        observer.onDependenciesResolved(runId, from, fromName, successors);
}

void ensureNodeIndexFitsSuccessorStorage(std::size_t nodeCount)
{
    if (nodeCount > std::numeric_limits<std::uint16_t>::max())
        SUB0PIPELINE_THROW("Pipeline exceeds the 65536-job uint16_t index range");
}
} // namespace

// ── PoolSuccessors: pool/arena-backed successor list ──────────────────────────
// 12 bytes: stores up to 4 successor indices inline using uint16_t (node indices
// ≤ 65535 cover all realistic pipelines). A longer list moves to a block in a
// flat uint16_t arena owned by Pipeline::Impl, so the node itself never grows.
//
// Layout (12 bytes, align 2):
//   [0..1]  size_      -- bit[15] = pool-mode flag; bits[14:0] = count (max 32767)
//   [2..3]  poolIdx_   -- low 16 bits of the block's start in Impl::succPool_
//   [4..11] inline_[4] -- inline mode: the entries themselves
//                         pool mode:   [0] = block capacity, [1] = high 16 bits
//                                      of the block's start, [2..3] unused
//
// A full block doubles. If it is the last block in the arena it grows in place;
// otherwise the entries move to a new block at the end and the old one is
// abandoned. Abandoned blocks sum to less than the live block, so the arena
// holds under four entries per stored edge.

struct PoolSuccessors
{
    static constexpr uint16_t kInlineCap = 4U;
    static constexpr uint16_t kPoolFlag  = 0x8000U; ///< bit[15] = pool mode
    static constexpr uint16_t kCountMask = 0x7FFFU; ///< bits[14:0] = count

    uint16_t size_{0};      ///< bit[15] = pool flag; bits[14:0] = entry count
    uint16_t poolIdx_{0};   ///< low half of the block start (pool mode only)
    uint16_t inline_[kInlineCap]{};  ///< entries, or block metadata in pool mode

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

    // Append a successor index. May grow `pool`, which invalidates pointers
    // previously returned by begin()/range() for any node.
    void push_back(uint16_t v, std::vector<uint16_t>& pool)
    {
        const uint16_t n = count();
        if (!isPool())
        {
            if (n < kInlineCap)
            {
                inline_[n] = v;
                size_ = static_cast<uint16_t>(n + 1U);
                return;
            }
            // Spill: the inline entries become the head of a new block.
            const std::size_t start = pool.size();
            pool.resize(start + 2U * kInlineCap);
            for (uint16_t i = 0; i < n; ++i) pool[start + i] = inline_[i];
            setBlock(start, static_cast<uint16_t>(2U * kInlineCap));
            size_ = static_cast<uint16_t>(kPoolFlag | n);
        }
        if (n == kCountMask)
            SUB0PIPELINE_THROW("A job cannot have more than 32767 successors");

        std::size_t start = blockStart();
        const uint16_t capacity = blockCapacity();
        if (n == capacity)
        {
            const auto grown = static_cast<uint16_t>(capacity > kCountMask / 2U
                ? kCountMask : capacity * 2U);
            if (start + capacity == pool.size())
            {
                pool.resize(start + grown);   // last block: extend in place
            }
            else
            {
                const std::size_t moved = pool.size();
                pool.resize(moved + grown);
                for (uint16_t i = 0; i < n; ++i) pool[moved + i] = pool[start + i];
                start = moved;
            }
            setBlock(start, grown);
        }
        pool[start + n] = v;
        size_ = static_cast<uint16_t>(kPoolFlag | (n + 1U));
    }

    // Returns a pointer into either inline_ or Impl::succPool_.
    // Pool pointer is valid until Impl::succPool_ is next modified (reallocated).
    const uint16_t* begin(const std::vector<uint16_t>& pool) const noexcept
    { return isPool() ? pool.data() + blockStart() : inline_; }
    const uint16_t* end(const std::vector<uint16_t>& pool)   const noexcept
    { return begin(pool) + count(); }

    // Range adapter for range-based for: `for (auto s : nd.successors_.range(pool))`
    struct Range
    {
        const uint16_t* b_; const uint16_t* e_;
        const uint16_t* begin() const noexcept { return b_; }
        const uint16_t* end()   const noexcept { return e_; }
    };
    Range range(const std::vector<uint16_t>& pool) const noexcept
    { return {begin(pool), end(pool)}; }

private:
    [[nodiscard]] std::size_t blockStart() const noexcept
    { return static_cast<std::size_t>(poolIdx_) | (static_cast<std::size_t>(inline_[1]) << 16U); }
    [[nodiscard]] uint16_t blockCapacity() const noexcept { return inline_[0]; }

    void setBlock(std::size_t start, uint16_t capacity) noexcept
    {
        poolIdx_   = static_cast<uint16_t>(start & 0xFFFFU);
        inline_[1] = static_cast<uint16_t>(start >> 16U);
        inline_[0] = capacity;
    }
};
static_assert(sizeof(PoolSuccessors) == 12,
    "PoolSuccessors must be exactly 12 bytes");

// ── Deferred jobs ─────────────────────────────────────────────────────────────

namespace detail
{

// Completions still held by deferred jobs that finished as cancelled or timed
// out. Owned by Pipeline::Impl, which joins them before it is destroyed.
struct DeferredOrphans
{
    std::mutex              mtx;
    std::condition_variable released;
    std::atomic<std::size_t> count{0U}; ///< Written under mtx; read lock-free.

    void release() noexcept
    {
        std::lock_guard lock{mtx};
        count.fetch_sub(1U, std::memory_order_release);
        released.notify_all();
    }

    bool wait() noexcept
    {
        std::unique_lock lock{mtx};
        if (count.load(std::memory_order_acquire) == 0U) return false;
        released.wait(lock, [this] { return count.load(std::memory_order_acquire) == 0U; });
        return true;
    }
};

// Per-job state of a deferred job. Three parties act on one run: the body
// returning, the Completion being signalled, and the stop source firing.
// Signal and stop race for the outcome; the body and the outcome winner
// each arrive once, and the second arrival finishes the job.
struct DeferredSlot
{
    static constexpr uint8_t kNone      = 0U;
    static constexpr uint8_t kSignalled = 1U;
    static constexpr uint8_t kStopped   = 2U;
    static constexpr uint8_t kReleased  = 0x01U; ///< The Completion was completed or destroyed.
    static constexpr uint8_t kOrphaned  = 0x02U; ///< Counted in DeferredOrphans until released.

    using Deliver = void (*)(void* run, uint32_t idx) noexcept;

    struct StopForward
    {
        DeferredSlot* slot;
        void operator()() const noexcept { slot->stop(); }
    };

    std::function<void(Completion)> start;
    DeferredOrphans*                orphans{nullptr}; ///< non-owning; Pipeline::Impl's ledger

    // Reset by the thread that runs the body, before the Completion exists.
    std::atomic<uint8_t>               outcome{kNone};
    std::atomic<uint8_t>               parties{0U};
    std::atomic<uint8_t>               handleFlags{0U};
    std::expected<void, PipelineError> result;
    std::string                        message;
    std::stop_token                    token;
    std::optional<std::stop_callback<StopForward>> stopCallback;
    std::optional<Deadline>            deadline;
    IDeadlineService*                  deadlineService{nullptr}; ///< Set while a registration may be live.
    void*                              run{nullptr};             ///< non-owning; the active run
    Deliver                            deliver{nullptr};
    uint32_t                           idx{0U};

    Completion handle() noexcept { return Completion{this}; }

    void signal(const std::expected<void, PipelineError>& value, std::string_view text) noexcept
    {
        auto none = kNone;
        if (!outcome.compare_exchange_strong(none, kSignalled, std::memory_order_acq_rel))
        {
            releaseLate();
            return;
        }
        // An expiry that came first owns the result, as for cooperative jobs.
        if (deadline && deadline->expired()) result = std::unexpected(PipelineError::kTimeout);
        else result = value;
        if (!value && !text.empty()) message.assign(text);
        arrive();
    }

    void stop() noexcept
    {
        auto none = kNone;
        if (!outcome.compare_exchange_strong(none, kStopped, std::memory_order_acq_rel)) return;
        result = std::unexpected(deadline && deadline->expired() ? PipelineError::kTimeout
                                                                 : PipelineError::kCancelled);
        arrive();
    }

    // The outcome winner's arrival. Nothing here may touch the slot afterwards:
    // the finisher can complete the run and a new run can reset the slot.
    void arrive() noexcept
    {
        if (parties.fetch_sub(1U, std::memory_order_acq_rel) == 1U) deliver(run, idx);
    }

    void releaseLate() noexcept
    {
        if ((handleFlags.fetch_or(kReleased, std::memory_order_acq_rel) & kOrphaned) != 0U)
            orphans->release();
    }

    // Called by the finisher of a stopped job. The count rises before the flag
    // is visible, so a releasing Completion never decrements below zero.
    void orphanIfHeld() noexcept
    {
        std::lock_guard lock{orphans->mtx};
        auto flags = handleFlags.load(std::memory_order_acquire);
        while ((flags & kReleased) == 0U)
        {
            if (handleFlags.compare_exchange_weak(flags, flags | kOrphaned, std::memory_order_acq_rel))
            {
                orphans->count.fetch_add(1U, std::memory_order_release);
                return;
            }
        }
    }

    // Never call from inside this job's stop callback or deadline expiry:
    // both destructors wait for those to return.
    void releaseRegistrations() noexcept
    {
        stopCallback.reset();
        if (auto* service = std::exchange(deadlineService, nullptr)) service->cancelAndWait(*deadline);
    }
};

} // namespace detail

Completion& Completion::operator=(Completion&& other) noexcept
{
    if (this != &other)
    {
        if (auto* held = std::exchange(slot_, nullptr))
            held->signal(std::unexpected(PipelineError::kJobFailed), "completion abandoned");
        slot_ = std::exchange(other.slot_, nullptr);
    }
    return *this;
}

Completion::~Completion()
{
    if (slot_) slot_->signal(std::unexpected(PipelineError::kJobFailed), "completion abandoned");
}

void Completion::complete(std::expected<void, PipelineError> result, std::string_view message) noexcept
{
    if (auto* held = std::exchange(slot_, nullptr)) held->signal(result, message);
}

std::stop_token Completion::stopToken() const noexcept
{
    return slot_ ? slot_->token : std::stop_token{};
}

namespace
{
// Serves deferred-job timeouts when no IDeadlineService is injected: one
// thread per Pipeline, started on first use, instead of one per pending job.
class TimerService final : public IDeadlineService
{
public:
    TimerService() : thread_{[this](std::stop_token stop) { loop(stop); }} {}

    bool arm(Deadline& deadline, std::chrono::milliseconds delay) noexcept override
    {
        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();
        const auto headroom = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
        const auto at = delay >= headroom ? Clock::time_point::max() : now + delay;
        {
            std::lock_guard lock{mtx_};
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            try
            {
#endif
                index_.emplace(&deadline, queue_.emplace(at, &deadline));
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            }
            catch (...)
            {
                return false;
            }
#endif
        }
        wake_.notify_all();
        return true;
    }

    void cancelAndWait(Deadline& deadline) noexcept override
    {
        std::unique_lock lock{mtx_};
        if (auto found = index_.find(&deadline); found != index_.end())
        {
            queue_.erase(found->second);
            index_.erase(found);
        }
        fired_.wait(lock, [&] { return firing_ != &deadline; });
    }

private:
    using Queue = std::multimap<std::chrono::steady_clock::time_point, Deadline*>;

    void loop(std::stop_token stop)
    {
        std::unique_lock lock{mtx_};
        while (!stop.stop_requested())
        {
            if (queue_.empty())
            {
                wake_.wait(lock, stop, [&] { return !queue_.empty(); });
                continue;
            }
            const auto at = queue_.begin()->first;
            if (std::chrono::steady_clock::now() < at)
            {
                wake_.wait_until(lock, stop, at, [&] { return queue_.empty() || queue_.begin()->first < at; });
                continue;
            }
            Deadline* due = queue_.begin()->second;
            index_.erase(due);
            queue_.erase(queue_.begin());
            firing_ = due;
            // expire() runs stop callbacks, which may call arm() or cancelAndWait().
            lock.unlock();
            due->expire();
            lock.lock();
            firing_ = nullptr;
            fired_.notify_all();
        }
    }

    std::mutex                                    mtx_;
    std::condition_variable_any                   wake_;
    std::condition_variable_any                   fired_;
    Queue                                         queue_;
    std::unordered_map<Deadline*, Queue::iterator> index_;
    Deadline*                                     firing_{nullptr};
    std::jthread                                  thread_; // last: stops before the state it uses
};
} // namespace

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
    uint8_t                 pad0_[1]{};         ///< Align the following integer fields.
    int                     coreAffinity_{-1};
    uint32_t                stackBytes_{8192U};
    uint32_t                predecessorCount_{0U}; ///< Immutable dependency count used during run initialization.
    std::chrono::milliseconds timeout_{std::chrono::milliseconds::max()};
    PoolSuccessors          successors_;            ///< Iterated on completion -- kept hot (12 B, inline ≤4, pool up to 32767).

    // ── Dispatch section: read once at the start of each job execution ────
    std::function<std::expected<void, PipelineError>(std::stop_token)> fn_;
    std::stop_source        stopSource_;

    // ── Cold section: build-time only ────────────────────────────────────
    std::string             nameStr_;
    const char*             statusText_{nullptr}; ///< non-owning; borrowed from the caller
    std::unique_ptr<detail::DeferredSlot> deferred_; ///< Set for emplaceDeferred() jobs only.

    // ── Flag constants ────────────────────────────────────────────────────
    static constexpr uint8_t kFlagOptional    = 0x01; ///< Failure does not block dependents.
    static constexpr uint8_t kFlagOnDemand    = 0x02; ///< Excluded from run() root set.
    static constexpr uint8_t kFlagCancellable = 0x04; ///< fn_ checks stop_token cooperatively.
    static constexpr uint8_t kFlagDeferred    = 0x08; ///< Completes through deferred_, not fn_.

    bool isOptional()    const noexcept { return (flags_ & kFlagOptional)    != 0U; }
    bool isOnDemand()    const noexcept { return (flags_ & kFlagOnDemand)    != 0U; }
    bool isCancellable() const noexcept { return (flags_ & kFlagCancellable) != 0U; }
    bool isDeferred()    const noexcept { return (flags_ & kFlagDeferred)    != 0U; }

    Node() = default;

    // std::atomic is non-copyable/non-movable, but std::vector may reallocate
    // nodes during the build phase. Loading in the move constructor is safe
    // because nodes are only moved before execution begins.
    Node(Node&& o) noexcept
        : unmetDeps_{o.unmetDeps_.load(std::memory_order_relaxed)}
        , jobStatus_{o.jobStatus_.load(std::memory_order_relaxed)}
        , flags_{o.flags_}
        , priority_{o.priority_}
        , coreAffinity_{o.coreAffinity_}
        , stackBytes_{o.stackBytes_}
        , predecessorCount_{o.predecessorCount_}
        , timeout_{o.timeout_}
        , successors_{std::move(o.successors_)}   // PoolSuccessors move ctor
        , fn_{std::move(o.fn_)}
        , stopSource_{std::move(o.stopSource_)}
        , nameStr_{std::move(o.nameStr_)}
        , statusText_{o.statusText_}
        , deferred_{std::move(o.deferred_)}
    {}

    Node& operator=(Node&&)      = delete;
    Node(const Node&)            = delete;
    Node& operator=(const Node&) = delete;
};

// ── Pipeline implementation structure ─────────────────────────────────────────

struct Pipeline::Impl : detail::PipelineAnchor
{
    using JobFn = std::function<std::expected<void, PipelineError>(std::stop_token)>;

    // Raise only for an intentional new field; do not raise to paper over bloat.
    static_assert(sizeof(Node) <= 168,
        "Pipeline::Node exceeded size budget -- check padding or new large fields");

    // Build-time / init-time fields (written once, read during run setup).
    std::vector<Node>         nodes_;
    std::vector<uint32_t>     roots_;
    // Graph-sized scratch retains capacity across validations/runs. Validation
    // has its own lock; failure traversal is serialized by DispatchContext.
    mutable std::mutex       validationMtx_;
    mutable std::vector<uint32_t> validationDegrees_, validationReady_;
    std::vector<uint32_t>     skipped_;
    // Ready jobs in the order they became ready, for executors that run
    // inline. Graph-sized scratch, retained across runs like skipped_.
    std::vector<uint32_t>     inlineReady_;
    std::vector<uint16_t>     succPool_;    ///< Flat arena for overflow successor indices (see PoolSuccessors).
    std::atomic_flag          running_ = ATOMIC_FLAG_INIT;
    std::mutex                stopMtx_;
    bool                      rootsCached_{false};
    IDeadlineService*          deadlines_{nullptr};
    IExecutor*                armedExecutor_{nullptr};
    IObserver*                armedObserver_{nullptr};

    // Written once under fatalMtx for the first fatal body/cancellation/submission error.
    // Readable after run() returns via Pipeline::firstFailureName().
    std::string               failedJobName_;

    // Hot atomic: written by every completing job thread on every run.
    // alignas(64) isolates it from the fields above so concurrent job-thread
    // writes to completedCount_ do not cause false sharing with runEpoch_ or
    // rootsCached_ which the run-owning thread reads/writes.
    alignas(64) std::atomic<uint32_t> completedCount_{0U};

    // ── Orphaned timeout threads (structured-cancellation groundwork) ─────
    // A non-cancellable job that exceeds .timeout() cannot be cooperatively
    // signalled or force-terminated, so its std::thread can no longer be
    // silently detach()'d -- that is exactly the "safe to free borrowed
    // state" ambiguity from issue #1. It is retained here, still joinable,
    // until joinOrphans() reaps it. See Pipeline::joinOrphans() docs.
    mutable std::mutex        orphanMtx_;
    std::vector<std::jthread> orphanThreads_;
    std::mutex               joinMtx_;
    std::atomic<std::size_t>  pendingOrphans_{0};

    std::vector<uint32_t>     deferredNodes_;
    // Deferred jobs finished on another thread, for the run thread of an
    // inline run; sized to the deferred job count and retained across runs.
    std::vector<uint32_t>     deferredFinished_, deferredDrain_;
    detail::DeferredOrphans   deferredOrphans_;
    // Wakes the run thread as deferred jobs finish; owned here rather than by
    // the run so that runs without deferred jobs do not construct them.
    std::mutex                deferredMtx_;
    std::condition_variable   deferredCv_;
    std::mutex                timerMtx_;
    std::unique_ptr<TimerService> timer_;

    ~Impl() { joinOrphans(); }

    // A moved-from Pipeline has no Impl; mutating calls recreate it.
    static Impl& ensure(std::unique_ptr<Impl>& impl, Pipeline* owner)
    {
        if (!impl)
        {
            impl = std::make_unique<Impl>();
            impl->owner = owner;
        }
        return *impl;
    }

    // Adapt a plain job to the stop-token signature. The token is ignored, so
    // kFlagCancellable stays clear and timeouts use the hard cutoff.
    static JobFn ignoringStop(std::function<std::expected<void, PipelineError>()> fn)
    {
        return [f = std::move(fn)](std::stop_token) { return f(); };
    }

    // Append a node named "<namePrefix><index>". Any new node may be a root
    // (on-demand nodes must leave the root set), so the cached roots are dropped.
    uint32_t addNode(JobFn fn, std::string_view namePrefix, uint8_t flags)
    {
        ensureNodeIndexFitsSuccessorStorage(nodes_.size());
        const auto idx = static_cast<uint32_t>(nodes_.size());
        auto& nd    = nodes_.emplace_back();
        nd.fn_      = std::move(fn);
        nd.flags_   = flags;
        nd.nameStr_ = std::string{namePrefix} + std::to_string(idx);
        rootsCached_ = false;
        return idx;
    }

    bool joinOrphans()
    {
        const bool reaped = joinOrphanThreads();
        return deferredOrphans_.wait() || reaped;
    }

    [[nodiscard]] bool hasOrphans() const noexcept
    {
        return pendingOrphans_.load(std::memory_order_acquire) != 0U
            || deferredOrphans_.count.load(std::memory_order_acquire) != 0U;
    }

    // Returns nullptr if the fallback timer cannot be started.
    IDeadlineService* deferredDeadlines() noexcept
    {
        if (deadlines_) return deadlines_;
        std::lock_guard lock{timerMtx_};
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        try
        {
#endif
            if (!timer_) timer_ = std::make_unique<TimerService>();
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        }
        catch (...)
        {
            return nullptr;
        }
#endif
        return timer_.get();
    }

    bool joinOrphanThreads()
    {
        if (pendingOrphans_.load(std::memory_order_acquire) == 0) return false;
        // Serialize joiners, but never hold the registry lock while waiting.
        std::lock_guard joining{joinMtx_};
        std::vector<std::jthread> batch;
        {
            std::lock_guard lock{orphanMtx_};
            batch.swap(orphanThreads_);
        }
        for (auto& thread : batch) thread.join();
        pendingOrphans_.fetch_sub(batch.size(), std::memory_order_release);
        return !batch.empty();
    }

    auto stopSource(Node& node)
    {
        std::lock_guard lock{stopMtx_};
        return node.stopSource_;
    }

    auto invokeWithDeadline(Node& node) -> std::expected<void, PipelineError>
    {
        Deadline deadline{node.stopSource_};
        if (!deadlines_->arm(deadline, node.timeout_))
            return std::unexpected(PipelineError::kDeadlineUnavailable);
        struct Registration
        {
            IDeadlineService& service;
            Deadline& deadline;
            bool active = true;
            void reset() noexcept
            {
                if (std::exchange(active, false)) service.cancelAndWait(deadline);
            }
            ~Registration() { reset(); }
        } registration{*deadlines_, deadline};
        const auto token = node.stopSource_.get_token();
        if (token.stop_requested())
        {
            registration.reset();
            return std::unexpected(deadline.expired() ? PipelineError::kTimeout
                                                     : PipelineError::kCancelled);
        }
        if (node.isCancellable())
        {
            auto result = node.fn_(token);
            registration.reset();
            return deadline.expired() ? std::unexpected(PipelineError::kTimeout) : result;
        }
        // The worker may outlive dispatch, so its completion state is owned.
        struct Completion
        {
            std::mutex mutex;
            std::condition_variable_any ready;
            bool done = false;
        };
        auto completion = std::make_shared<Completion>();
        std::packaged_task<std::expected<void, PipelineError>()> task{
            [fn = node.fn_, token] { return fn(token); }
        };
        auto result = task.get_future();
        std::jthread thread{[task = std::move(task), completion]() mutable {
            task();
            std::lock_guard lock{completion->mutex};
            completion->done = true;
            completion->ready.notify_all();
        }};
        bool done;
        {
            std::unique_lock lock{completion->mutex};
            done = completion->ready.wait(lock, token, [&] { return completion->done; });
        }
        registration.reset();
        if (done)
        {
            thread.join();
            auto value = result.get();
            return deadline.expired() ? std::unexpected(PipelineError::kTimeout) : value;
        }
        retain(std::move(thread));
        return std::unexpected(deadline.expired() ? PipelineError::kTimeout
                                                 : PipelineError::kCancelled);
    }

    void retain(std::jthread thread)
    {
        std::lock_guard lock{orphanMtx_};
        orphanThreads_.push_back(std::move(thread));
        pendingOrphans_.fetch_add(1, std::memory_order_release);
    }

    auto invoke(Node& node) -> std::expected<void, PipelineError>
    {
        if (node.stopSource_.stop_requested())
            return std::unexpected(PipelineError::kCancelled);

        if (node.timeout_ == std::chrono::milliseconds::max())
        {
            // A plain job's wrapper discards its token, so skip the shared
            // state's reference count and hand it an empty one.
            return node.fn_(node.isCancellable() ? node.stopSource_.get_token()
                                                 : std::stop_token{});
        }
        return invokeTimed(node, node.stopSource_.get_token());
    }

    // Give the node cancellation state no earlier run can reach. A cancellable
    // or deferred job was handed a token and may have kept it, so it always
    // gets new state.
    // A plain job never sees one: its state is only reachable from inside the
    // library, so it is kept until a stop has actually been requested on it.
    static void renewStopState(Node& node)
    {
        if (node.isCancellable() || node.isDeferred() || node.stopSource_.stop_requested())
            node.stopSource_ = std::stop_source{};
    }

    // Keep opt-in machinery out of the small untimed invocation path so that
    // compilers can inline the cancellation gate and ordinary callable dispatch.
    auto invokeTimed(Node& node, const std::stop_token& token)
        -> std::expected<void, PipelineError>
    {
        if (deadlines_) return invokeWithDeadline(node);

        if (node.isCancellable())
        {
            std::atomic<bool> expired{false};
            // Interrupt the deadline wait when the job finishes or is cancelled.
            std::jthread watchdog{[source = node.stopSource_, duration = node.timeout_, &expired]
                (std::stop_token finished) mutable {
                std::mutex mutex;
                std::condition_variable_any wake;
                std::stop_callback cancelled{source.get_token(), [&] {
                    std::lock_guard lock{mutex};
                    wake.notify_all();
                }};
                std::unique_lock lock{mutex};
                const bool stopped = wake.wait_for(lock, finished, duration,
                              [&] { return source.stop_requested(); });
                lock.unlock();
                if (!stopped && !finished.stop_requested())
                {
                    expired.store(true, std::memory_order_release);
                    source.request_stop();
                }
            }};
            auto result = node.fn_(token);
            watchdog.request_stop();
            watchdog.join();
            return expired.load(std::memory_order_acquire)
                ? std::unexpected(PipelineError::kTimeout) : result;
        }

        std::packaged_task<std::expected<void, PipelineError>()> task{
            [fn = node.fn_, token] { return fn(token); }
        };
        auto result = task.get_future();
        std::jthread thread{std::move(task)};
        if (result.wait_for(node.timeout_) != std::future_status::timeout)
        {
            thread.join();
            return result.get();
        }
        node.stopSource_.request_stop();
        retain(std::move(thread));
        return std::unexpected(PipelineError::kTimeout);
    }

};

void Pipeline::setDeadlineService(IDeadlineService* service)
{
    Impl::ensure(impl_, this).deadlines_ = service;
}

// ── Job builder methods ───────────────────────────────────────────────────────

Job& Job::name(std::string_view n)
{
    if (pipeline() && valid()) pipeline()->node(idx_).nameStr_ = std::string{n};
    return *this;
}

Job& Job::timeout(std::chrono::milliseconds t) noexcept
{
    if (pipeline() && valid()) pipeline()->node(idx_).timeout_ = t;
    return *this;
}

Job& Job::stack(uint32_t bytes) noexcept
{
    if (pipeline() && valid()) pipeline()->node(idx_).stackBytes_ = bytes;
    return *this;
}

Job& Job::core(int c) noexcept
{
    if (pipeline() && valid()) pipeline()->node(idx_).coreAffinity_ = c;
    return *this;
}

Job& Job::priority(uint8_t p) noexcept
{
    if (pipeline() && valid()) pipeline()->node(idx_).priority_ = p;
    return *this;
}

Job& Job::optional(bool opt) noexcept
{
    if (pipeline() && valid())
    {
        auto& nd = pipeline()->node(idx_);
        if (opt) nd.flags_ |= Pipeline::Node::kFlagOptional;
        else     nd.flags_ &= static_cast<uint8_t>(~Pipeline::Node::kFlagOptional);
    }
    return *this;
}

Job& Job::statusText(const char* text) noexcept
{
    if (pipeline() && valid()) pipeline()->node(idx_).statusText_ = text;
    return *this;
}

void Job::cancel() noexcept
{
    if (pipeline() && valid())
    {
        // Copy under the reset lock; invoke user stop callbacks outside it.
        auto source = pipeline()->impl_->stopSource(pipeline()->node(idx_));
        source.request_stop();
    }
}

Job& Job::succeed(Job other)
{
    if (!pipeline() || !valid() || !other.valid()) return *this;
    auto& selfNode  = pipeline()->node(idx_);
    auto& otherNode = pipeline()->node(other.idx_);
    pipeline()->impl_->rootsCached_ = false;
    // Record the edge first: if it is rejected, no dependency count is left behind.
    otherNode.successors_.push_back(static_cast<uint16_t>(idx_), pipeline()->impl_->succPool_);
    ++selfNode.predecessorCount_;
    return *this;
}

Job& Job::precede(Job other)
{
    if (!pipeline() || !valid() || !other.valid()) return *this;
    auto& selfNode  = pipeline()->node(idx_);
    auto& otherNode = pipeline()->node(other.idx_);
    pipeline()->impl_->rootsCached_ = false;
    selfNode.successors_.push_back(static_cast<uint16_t>(other.idx_), pipeline()->impl_->succPool_);
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

Pipeline::Pipeline() : impl_{std::make_unique<Impl>()} { impl_->owner = this; }
Pipeline::~Pipeline() = default;

// Job handles point at the Impl, which does not move; re-point it at the
// Pipeline object that now owns it so the handles follow.
Pipeline::Pipeline(Pipeline&& other) noexcept : impl_{std::move(other.impl_)}
{
    if (impl_) impl_->owner = this;
}

Pipeline& Pipeline::operator=(Pipeline&& other)
{
    impl_ = std::move(other.impl_);
    if (impl_) impl_->owner = this;
    return *this;
}

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
    return emplacePlain(Impl::ignoringStop(std::move(fn)));
}

Job Pipeline::emplace(
    std::function<std::expected<void, PipelineError>(std::stop_token)> fn)
{
    return Job{Impl::ensure(impl_, this).addNode(std::move(fn), "job_", Node::kFlagCancellable), impl_.get()};
}

Job Pipeline::emplacePlain(std::function<std::expected<void, PipelineError>(std::stop_token)> fn)
{
    return Job{Impl::ensure(impl_, this).addNode(std::move(fn), "job_", 0U), impl_.get()};
}

Job Pipeline::emplaceDeferred(std::function<void(Completion)> start)
{
    auto& impl = Impl::ensure(impl_, this);
    auto slot = std::make_unique<detail::DeferredSlot>();
    slot->start = std::move(start);
    slot->orphans = &impl.deferredOrphans_;
    // Reserve first: once the node exists, recording it must not fail.
    impl.deferredNodes_.reserve(impl.deferredNodes_.size() + 1U);
    const auto idx = impl.addNode({}, "job_", Node::kFlagDeferred);
    impl.nodes_[idx].deferred_ = std::move(slot);
    impl.deferredNodes_.push_back(idx);
    return Job{idx, impl_.get()};
}

void Pipeline::reserve(std::size_t jobCount)
{
    Impl::ensure(impl_, this).nodes_.reserve(jobCount);
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
    std::lock_guard lock{impl_->validationMtx_};
    auto& inDegree = impl_->validationDegrees_;
    auto& ready = impl_->validationReady_;
    inDegree.assign(n, 0U);
    ready.clear();
    ready.reserve(n);
    for (const auto& nd : impl_->nodes_)
    {
        for (auto succ : nd.successors_.range(impl_->succPool_))
            ++inDegree[succ];
    }

    for (uint32_t i = 0U; i < n; ++i)
    {
        if (inDegree[i] == 0U) ready.push_back(i);
    }

    uint32_t visited = 0U;
    while (visited < ready.size())
    {
        const auto idx = ready[visited];
        ++visited;
        for (auto succ : impl_->nodes_[idx].successors_.range(impl_->succPool_))
        {
            if (--inDegree[succ] == 0U) ready.push_back(succ);
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
    return runImpl(executor, {}, observer);
}

auto Pipeline::run(IExecutor& executor, std::stop_token external, IObserver* observer)
    -> std::expected<void, PipelineError>
{
    return runImpl(executor, external, observer);
}

auto Pipeline::runImpl(IExecutor& executor, std::stop_token external, IObserver* observer)
    -> std::expected<void, PipelineError>
{
    if (!impl_ || impl_->nodes_.empty()) return {};
    if (impl_->running_.test_and_set(std::memory_order_acquire))
        return std::unexpected(PipelineError::kBusy);
    struct RunGuard
    {
        std::atomic_flag& running;
        ~RunGuard() { running.clear(std::memory_order_release); }
    } running{impl_->running_};
    // A new epoch must not reuse borrowed state while an old job still runs.
    impl_->joinOrphans();
    const auto total = impl_->nodes_.size();
    impl_->completedCount_.store(0U, std::memory_order_relaxed);
    impl_->skipped_.clear();

    if (!impl_->rootsCached_)
    {
        if (auto result = validate(); !result) return result;
        impl_->roots_.clear();
        for (uint32_t i = 0U; i < static_cast<uint32_t>(total); ++i)
        {
            const auto& nd = impl_->nodes_[i];
            if (nd.predecessorCount_ == 0U && !nd.isOnDemand())
                impl_->roots_.push_back(i);
        }
        impl_->rootsCached_ = true;
    }

    std::size_t failureNameCapacity = 0U;
    {
        std::lock_guard lock{impl_->stopMtx_};
        for (auto& node : impl_->nodes_)
        {
            Impl::renewStopState(node);
            node.unmetDeps_.store(node.predecessorCount_, std::memory_order_relaxed);
            node.jobStatus_.store(node.predecessorCount_ == 0U && !node.isOnDemand()
                ? JobStatus::kReady : JobStatus::kPending, std::memory_order_relaxed);
            failureNameCapacity = std::max(failureNameCapacity, node.nameStr_.size());
        }
    }
    // Failure containment must still work when a submission ran out of memory.
    impl_->skipped_.reserve(total);
    impl_->failedJobName_.reserve(failureNameCapacity);
    // Each deferred job finishes once per run, so these never grow mid-run.
    impl_->deferredFinished_.clear();
    impl_->deferredFinished_.reserve(impl_->deferredNodes_.size());
    impl_->deferredDrain_.reserve(impl_->deferredNodes_.size());

    std::atomic<bool> hasFatalFailure{false};
    PipelineError     fatalError{PipelineError::kJobFailed};
    std::mutex        fatalMtx;
    impl_->failedJobName_.clear();

    // A local struct so it can name Pipeline's private Impl and Node types.
    // executor.dispatch() receives [&ctx, si] (pointer + uint32_t), which fits
    // std::function's small buffer: no heap allocation per dispatch.
    struct DispatchContext
    {
        Impl&               impl;
        Pipeline&           pipeline;
        IExecutor&          executor;
        IObserver*          observer;
        RunId               runId;
        uint32_t            total;
        std::atomic<bool>&  hasFatalFailure;
        PipelineError&      fatalError;
        std::mutex&         fatalMtx;
        std::vector<uint32_t>* inlineReady; ///< Set when the run calls jobs itself; else nullptr.

        // Deferred jobs started and not yet finished, guarded by
        // impl.deferredMtx_. A finish decrements it last, after dispatching
        // successors, so zero means the executor has already accounted for
        // every dispatch those jobs caused.
        uint32_t deferredInFlight = 0U;

        std::mutex skipMutex{};
        std::size_t skipRead = 0;
        bool drainingSkips = false;

        void recordFailure(uint32_t idx, PipelineError error)
        {
            std::lock_guard lock{fatalMtx};
            if (!hasFatalFailure.load(std::memory_order_relaxed))
            {
                fatalError = error;
                impl.failedJobName_ = impl.nodes_[idx].nameStr_;
                hasFatalFailure.store(true, std::memory_order_release);
            }
        }

        void submissionFailed(uint32_t idx)
        {
            auto& nd = impl.nodes_[idx];
            auto ready = JobStatus::kReady;
            const bool failed = nd.jobStatus_.compare_exchange_strong(
                ready, JobStatus::kFailed, std::memory_order_acq_rel);
            recordFailure(idx, PipelineError::kJobFailed);
            if (failed)
            {
                const auto done = impl.completedCount_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
                if (observer)
                {
                    observer->onJobFinish(runId, idx, nd.nameStr_, JobStatus::kFailed,
                                         static_cast<float>(done) / static_cast<float>(total));
                    observer->onJobFailure(runId, idx, nd.nameStr_, PipelineError::kJobFailed, {});
                    notifyDependencies(*observer, pipeline, runId, idx, nd.nameStr_);
                }
            }
            for (uint32_t pending = 0U; pending < total; ++pending) skipNode(pending);
        }

        // Called with skipMutex held. Claim before appending, so even shared
        // descendants appear at most once and scratch never exceeds total nodes.
        void enqueueSkip(uint32_t idx)
        {
            auto& status = impl.nodes_[idx].jobStatus_;
            auto previous = status.load(std::memory_order_acquire);
            while (previous == JobStatus::kPending || previous == JobStatus::kReady)
            {
                if (status.compare_exchange_weak(previous, JobStatus::kSkipped,
                                                  std::memory_order_acq_rel))
                {
                    impl.skipped_.push_back(idx);
                    return;
                }
            }
        }

        void skipNode(uint32_t startIdx)
        {
            std::unique_lock lock{skipMutex};
            enqueueSkip(startIdx);
            if (drainingSkips) return;
            drainingSkips = true;
            while (skipRead < impl.skipped_.size())
            {
                const auto idx = impl.skipped_[skipRead++];
                auto& nd = impl.nodes_[idx];
                for (auto succ : nd.successors_.range(impl.succPool_)) enqueueSkip(succ);
                const auto done = impl.completedCount_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
                const auto progress = static_cast<float>(done) / static_cast<float>(total);
                // Observers may cancel jobs or validate the graph. Never call
                // user code under a traversal lock. Other workers may enqueue;
                // this drainer remains an active executor callback until done.
                lock.unlock();
                if (observer)
                {
                    observer->onJobFinish(runId, idx, nd.nameStr_, JobStatus::kSkipped, progress);
                    notifyDependencies(*observer, pipeline, runId, idx, nd.nameStr_);
                }
                lock.lock();
            }
            drainingSkips = false;
        }

        void dispatchJob(uint32_t idx)
        {
            auto& nd = impl.nodes_[idx];
            if (hasFatalFailure.load(std::memory_order_acquire)) { skipNode(idx); return; }

            auto ready = JobStatus::kReady;
            if (!nd.jobStatus_.compare_exchange_strong(ready, JobStatus::kRunning,
                                                       std::memory_order_acq_rel)) return;
            if (observer) observer->onJobStart(runId, idx, nd.nameStr_);

            if (nd.isDeferred())
            {
                startDeferred(idx, nd);
                return;
            }
            auto result = impl.invoke(nd);
            // Consume the thread-local error context set by the job (failure branch only).
            // Move clears t_jobError so it does not leak into the next job on this thread.
            // On the success path t_jobError is empty and this move is a no-op.
            std::string jobErrorCtx = std::move(t_jobError);
            uint32_t submitting = kNoSubmission;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            try
            {
#endif
                finishJob(idx, nd, result, jobErrorCtx, submitting);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            }
            catch (...)
            {
                if (submitting == kNoSubmission) throw;
                submissionFailed(submitting);
            }
#endif
        }

        // The one finish path for every job. MSVC never inlines a function with
        // a try block, so the caller owns the executor-rejection boundary: a
        // dispatch that throws leaves its job index in @p submitting.
        SUB0PIPELINE_FORCEINLINE void finishJob(uint32_t idx, Node& nd,
                                                const std::expected<void, PipelineError>& result,
                                                std::string_view errorText, uint32_t& submitting)
        {
            const auto jobStatus = statusOf(result);
            nd.jobStatus_.store(jobStatus, std::memory_order_release);

            const auto done     = impl.completedCount_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
            const auto progress = static_cast<float>(done) / static_cast<float>(total);
            if (observer)
            {
                observer->onJobFinish(runId, idx, nd.nameStr_, jobStatus, progress);
                notifyDependencies(*observer, pipeline, runId, idx, nd.nameStr_);
            }

            if (!result && (!nd.isOptional() || result.error() == PipelineError::kCancelled))
            {
                // Report failure detail through the dedicated hook -- zero cost when
                // no observer is attached.
                if (observer)
                    observer->onJobFailure(runId, idx, nd.nameStr_, result.error(), errorText);

                recordFailure(idx, result.error());
                for (auto succIdx : nd.successors_.range(impl.succPool_)) skipNode(succIdx);
                return;
            }

            for (auto succIdx : nd.successors_.range(impl.succPool_))
            {
                if (hasFatalFailure.load(std::memory_order_acquire))
                {
                    skipNode(succIdx);
                    continue;
                }
                auto& succ = impl.nodes_[succIdx];
                const auto remaining = succ.unmetDeps_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
                if (remaining == 0U)
                {
                    auto pending = JobStatus::kPending;
                    if (!succ.jobStatus_.compare_exchange_strong(pending, JobStatus::kReady,
                                                                std::memory_order_acq_rel)) continue;
                    if (inlineReady)
                    {
                        inlineReady->push_back(succIdx);
                        continue;
                    }
                    submitting = succIdx;
                    executor.dispatch(
                        succ.nameStr_,
                        [this, si = succIdx]{ dispatchJob(si); },
                        [] {},
                        succ.coreAffinity_, succ.priority_, succ.stackBytes_);
                }
            }
        }

        // finishJob with its own rejection boundary, for the deferred paths.
        void finishCold(uint32_t idx, Node& nd, const std::expected<void, PipelineError>& result,
                        std::string_view errorText)
        {
            uint32_t submitting = kNoSubmission;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            try
            {
#endif
                finishJob(idx, nd, result, errorText, submitting);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            }
            catch (...)
            {
                if (submitting == kNoSubmission) throw;
                submissionFailed(submitting);
            }
#endif
        }


        static void deliverThunk(void* run, uint32_t idx) noexcept
        {
            static_cast<DispatchContext*>(run)->deliverDeferred(idx);
        }

        void startDeferred(uint32_t idx, Node& nd)
        {
            auto& slot = *nd.deferred_;
            if (nd.stopSource_.stop_requested())
            {
                finishCold(idx, nd, std::unexpected(PipelineError::kCancelled), {});
                return;
            }
            slot.outcome.store(detail::DeferredSlot::kNone, std::memory_order_relaxed);
            slot.parties.store(2U, std::memory_order_relaxed);
            slot.handleFlags.store(0U, std::memory_order_relaxed);
            slot.result = {};
            slot.message.clear();
            slot.token = nd.stopSource_.get_token();
            slot.run = this;
            slot.deliver = &deliverThunk;
            slot.idx = idx;
            slot.deadline.reset();
            if (nd.timeout_ != std::chrono::milliseconds::max())
            {
                auto* service = impl.deferredDeadlines();
                slot.deadline.emplace(nd.stopSource_);
                if (!service || !service->arm(*slot.deadline, nd.timeout_))
                {
                    slot.deadline.reset();
                    finishCold(idx, nd, std::unexpected(PipelineError::kDeadlineUnavailable), {});
                    return;
                }
                slot.deadlineService = service;
            }
            {
                std::lock_guard lock{impl.deferredMtx_};
                ++deferredInFlight;
            }
            // Runs stop() at once if a stop, or a synchronous expiry, came first.
            slot.stopCallback.emplace(slot.token, detail::DeferredSlot::StopForward{&slot});
            slot.start(slot.handle());

            // The finisher cannot run before this party arrives, so awaiting
            // always precedes the finish.
            if (slot.outcome.load(std::memory_order_acquire) == detail::DeferredSlot::kNone)
            {
                nd.jobStatus_.store(JobStatus::kAwaiting, std::memory_order_release);
                if (observer) observer->onJobAwaiting(runId, idx, nd.nameStr_);
            }
            if (slot.parties.fetch_sub(1U, std::memory_order_acq_rel) == 1U) finishDeferred(idx, true);
        }

        // Called by the signal or stop party when it arrives last: possibly a
        // transport, timer or stop-requesting thread the library does not own.
        void deliverDeferred(uint32_t idx) noexcept
        {
            if (inlineReady)
            {
                std::lock_guard lock{impl.deferredMtx_};
                impl.deferredFinished_.push_back(idx);
                impl.deferredCv_.notify_all();
                return;
            }
            auto& nd = impl.nodes_[idx];
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            try
            {
#endif
                executor.dispatch(nd.nameStr_, [this, idx] { finishDeferred(idx, true); }, [] {},
                                  nd.coreAffinity_, nd.priority_, nd.stackBytes_);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            }
            catch (...)
            {
                // This thread may be inside the stop callback or the expiry
                // that releasing would wait for; the run end releases them.
                finishDeferred(idx, false);
            }
#endif
        }

        void finishDeferred(uint32_t idx, bool releaseRegistrations) noexcept
        {
            auto& nd = impl.nodes_[idx];
            auto& slot = *nd.deferred_;
            if (releaseRegistrations) slot.releaseRegistrations();
            if (slot.outcome.load(std::memory_order_acquire) == detail::DeferredSlot::kStopped)
                slot.orphanIfHeld();
            finishCold(idx, nd, slot.result, slot.message);
            std::lock_guard lock{impl.deferredMtx_};
            --deferredInFlight;
            impl.deferredCv_.notify_all();
        }

        // Runs on the run thread of an inline run once its ready list is empty.
        // Returns false when no deferred job remains in flight.
        bool finishAwaitedInline()
        {
            auto& drain = impl.deferredDrain_;
            {
                std::unique_lock lock{impl.deferredMtx_};
                impl.deferredCv_.wait(lock, [&] {
                    return !impl.deferredFinished_.empty() || deferredInFlight == 0U;
                });
                if (impl.deferredFinished_.empty()) return false;
                drain.swap(impl.deferredFinished_);
            }
            for (auto idx : drain) finishDeferred(idx, true);
            drain.clear();
            return true;
        }

        // Threaded runs: continuations and their successors are dispatched
        // while the executor may already look idle, so wait for both together.
        void awaitDeferredThreaded()
        {
            for (;;)
            {
                {
                    std::unique_lock lock{impl.deferredMtx_};
                    impl.deferredCv_.wait(lock, [&] { return deferredInFlight == 0U; });
                }
                executor.waitAll();
                std::lock_guard lock{impl.deferredMtx_};
                if (deferredInFlight == 0U) return;
            }
        }
    };

    const RunId runId = observer ? observer->onRunStart() : 0U;
    DispatchContext ctx{*impl_, *this, executor, observer, runId,
                        static_cast<uint32_t>(total),
                        hasFatalFailure, fatalError, fatalMtx,
                        executor.runsInline() ? &impl_->inlineReady_ : nullptr};

    // Forward an external stop with one registration for the whole run, not
    // one per job. It reaches every node: jobs not yet started are refused at
    // entry, and running cooperative jobs see it through their own token.
    // Node stop sources are immutable from run initialization until this
    // registration is destroyed, which waits for a callback still in progress.
    struct ForwardStop
    {
        Impl& impl;
        void operator()() const noexcept
        {
            for (auto& node : impl.nodes_) node.stopSource_.request_stop();
        }
    };
    std::optional<std::stop_callback<ForwardStop>> forwardStop;
    if (external.stop_possible()) forwardStop.emplace(external, ForwardStop{*impl_});

    if (ctx.inlineReady)
    {
        // The executor would only call each job on this thread, so call them
        // here from a worklist. A completing job appends its ready successors
        // instead of running them, which keeps stack depth independent of
        // chain length. Each job is appended once, so the reserve is exact.
        auto& ready = *ctx.inlineReady;
        ready.reserve(total);
        ready.assign(impl_->roots_.begin(), impl_->roots_.end());
        std::size_t next = 0;
        do
        {
            for (; next < ready.size(); ++next) ctx.dispatchJob(ready[next]);
        } while (!impl_->deferredNodes_.empty() && ctx.finishAwaitedInline());
    }
    else
    {
        for (auto idx : impl_->roots_)
        {
            if (hasFatalFailure.load(std::memory_order_acquire))
            {
                ctx.skipNode(idx);
                continue;
            }
            auto& nd = impl_->nodes_[idx];
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            try
            {
#endif
                executor.dispatch(
                    nd.nameStr_,
                    [&ctx, i = idx]{ ctx.dispatchJob(i); },
                    [] {},
                    nd.coreAffinity_, nd.priority_, nd.stackBytes_);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            }
            catch (...)
            {
                ctx.submissionFailed(idx);
                break;
            }
#endif
        }
    }

    executor.waitAll();
    if (!impl_->deferredNodes_.empty())
    {
        if (!ctx.inlineReady) ctx.awaitDeferredThreaded();
        for (auto idx : impl_->deferredNodes_) impl_->nodes_[idx].deferred_->releaseRegistrations();
    }

    if (hasFatalFailure.load(std::memory_order_acquire))
    {
        std::lock_guard lk{fatalMtx};
        return std::unexpected(fatalError);
    }
    return {};
}

// ── Status queries ────────────────────────────────────────────────────────────

auto DependencyRange::Iterator::operator*() const noexcept -> Target
{
    const auto id = static_cast<JobId>(ids_[index_]);
    return {id, pipeline_->name(id)};
}

auto Pipeline::status(Job j) const noexcept -> JobStatus
{
    if (!impl_ || j.pipeline() != this || !j.valid() || j.idx_ >= impl_->nodes_.size())
        return JobStatus::kPending;
    return impl_->nodes_[j.idx_].jobStatus_.load(std::memory_order_acquire);
}

auto Pipeline::name(Job j) const noexcept -> std::string_view
{
    if (!impl_ || j.pipeline() != this || !j.valid() || j.idx_ >= impl_->nodes_.size())
        return {};
    return impl_->nodes_[j.idx_].nameStr_;
}

auto Pipeline::name(JobId id) const noexcept -> std::string_view
{
    if (!impl_ || id >= impl_->nodes_.size())
        return {};
    return impl_->nodes_[id].nameStr_;
}

auto Pipeline::successors(JobId id) const noexcept -> DependencyRange
{
    if (!impl_ || id >= impl_->nodes_.size())
        return {};
    const auto range = impl_->nodes_[id].successors_.range(impl_->succPool_);
    return DependencyRange{this, range.begin(), impl_->nodes_[id].successors_.size()};
}

std::string_view Pipeline::firstFailureName() const noexcept
{
    if (!impl_) return {};
    return impl_->failedJobName_;
}

void Pipeline::setCurrentJobError(std::string_view msg) noexcept
{
    // Called from within a job function on the failure branch.
    // Allocates only here (failure path); the success path never calls this.
    t_jobError = msg;
}

auto Pipeline::statusText(JobId id) const noexcept -> std::string_view
{
    if (!impl_ || id >= impl_->nodes_.size()) return {};
    const char* text = impl_->nodes_[id].statusText_;
    return text ? std::string_view{text} : std::string_view{};
}

std::vector<Pipeline::JobSnapshot> Pipeline::snapshot() const
{
    if (!impl_) return {};
    std::vector<JobSnapshot> out;
    out.reserve(impl_->nodes_.size());
    for (const auto& nd : impl_->nodes_)
    {
        out.push_back({nd.nameStr_, nd.jobStatus_.load(std::memory_order_relaxed),
                       nd.statusText_ ? std::string_view{nd.statusText_} : std::string_view{}});
    }
    return out;
}

namespace
{
struct InlineExecutor final : IExecutor
{
    void dispatch(std::string_view, std::function<void()> fn,
                  std::function<void()> oc, int, uint8_t, uint32_t) override
    { fn(); if (oc) oc(); }
    void waitAll() override {}
    [[nodiscard]] int concurrency() const noexcept override { return 1; }
    [[nodiscard]] bool runsInline() const noexcept override { return true; }
};
} // namespace

auto Pipeline::runInline(IObserver* observer) -> std::expected<void, PipelineError>
{
    InlineExecutor exec;
    return run(exec, observer);
}

auto Pipeline::runInline(std::stop_token external, IObserver* observer)
    -> std::expected<void, PipelineError>
{
    InlineExecutor exec;
    return run(exec, std::move(external), observer);
}

bool Pipeline::joinOrphans()
{
    return impl_ && impl_->joinOrphans();
}

bool Pipeline::hasPendingOrphans() const noexcept
{
    return impl_ && impl_->hasOrphans();
}

void Pipeline::runUntil(IExecutor& executor, std::stop_token stop,
                         IObserver* observer,
                         std::function<void(PipelineError)> onError)
{
    while (!stop.stop_requested())
    {
        auto result = run(executor, stop, observer);
        if (!result && onError)
            onError(result.error());
    }
}

// ── On-demand jobs ────────────────────────────────────────────────────────────

void Pipeline::arm(IExecutor& executor, IObserver* observer) noexcept
{
    auto& impl = Impl::ensure(impl_, this);
    impl.armedExecutor_ = &executor;
    impl.armedObserver_ = observer;
}

Job Pipeline::addOnDemand(std::function<std::expected<void, PipelineError>()> fn)
{
    return Job{Impl::ensure(impl_, this).addNode(Impl::ignoringStop(std::move(fn)), "on_demand_",
                                           Node::kFlagOnDemand), impl_.get()};
}

Job Pipeline::addOnDemand(
    std::function<std::expected<void, PipelineError>(std::stop_token)> fn)
{
    return Job{Impl::ensure(impl_, this).addNode(std::move(fn), "on_demand_",
                                           Node::kFlagOnDemand | Node::kFlagCancellable), impl_.get()};
}

auto Pipeline::trigger(Job j) -> std::expected<void, PipelineError>
{
    if (!impl_ || j.pipeline() != this || !j.valid() || j.idx_ >= impl_->nodes_.size())
        return std::unexpected(PipelineError::kUnknownJob);

    if (!impl_->armedExecutor_)
        return std::unexpected(PipelineError::kNotArmed);

    auto& nd = impl_->nodes_[j.idx_];
    if (!nd.isOnDemand())
        return std::unexpected(PipelineError::kNotOnDemand);

    if (impl_->running_.test(std::memory_order_acquire) || hasPendingOrphans())
        return std::unexpected(PipelineError::kBusy);
    {
        std::lock_guard lock{impl_->stopMtx_};
        const auto status = nd.jobStatus_.load(std::memory_order_acquire);
        if (status == JobStatus::kReady || status == JobStatus::kRunning)
            return std::unexpected(PipelineError::kBusy);
        Impl::renewStopState(nd);
        nd.jobStatus_.store(JobStatus::kReady, std::memory_order_release);
    }

    IExecutor*  exec = impl_->armedExecutor_;
    IObserver*  obs  = impl_->armedObserver_;

    // Capture by index, not by nd reference: nodes_ may reallocate if another
    // addOnDemand() is called before the dispatch lambda executes.
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        exec->dispatch(
        nd.nameStr_,
        [impl = impl_.get(), pipeline = this, idx = j.idx_, obs]
        {
            auto& n = impl->nodes_[idx];
            n.jobStatus_.store(JobStatus::kRunning, std::memory_order_release);
            const RunId runId = obs ? obs->onRunStart() : 0U;
            if (obs) obs->onJobStart(runId, idx, n.nameStr_);

            auto result = impl->invoke(n);

            const auto status = statusOf(result);
            n.jobStatus_.store(status, std::memory_order_release);
            auto jobErrorCtx = std::move(t_jobError);
            if (obs)
            {
                obs->onJobFinish(runId, idx, n.nameStr_, status, 1.0f);
                notifyDependencies(*obs, *pipeline, runId, idx, n.nameStr_);
                if (!result && (!n.isOptional() ||
                                result.error() == PipelineError::kCancelled))
                {
                    obs->onJobFailure(runId, idx, n.nameStr_, result.error(), jobErrorCtx);
                }
            }
        },
        [] {},
        nd.coreAffinity_,
        nd.priority_,
        nd.stackBytes_);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (...)
    {
        nd.jobStatus_.store(JobStatus::kFailed, std::memory_order_release);
        return std::unexpected(PipelineError::kJobFailed);
    }
#endif

    return {};
}

// ── Diagnostics ───────────────────────────────────────────────────────────────

void Pipeline::dumpText(std::ostream& output) const
{
    if (!impl_) return;
    output << "Pipeline DAG (" << impl_->nodes_.size() << " jobs):\n";
    for (uint32_t i = 0U; i < static_cast<uint32_t>(impl_->nodes_.size()); ++i)
    {
        const auto& nd = impl_->nodes_[i];
        output << "  [" << i << "] " << nd.nameStr_ << " (predecessors: "
               << nd.predecessorCount_ << ") -> (";
        for (auto successor : nd.successors_.range(impl_->succPool_))
            output << ' ' << impl_->nodes_[successor].nameStr_;
        output << ")\n";
    }
}

} // namespace sub0pipeline
