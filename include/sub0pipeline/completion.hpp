// include/sub0pipeline/completion.hpp
//
// Completion -- the move-only handle that finishes a deferred job.
#pragma once

#include <expected>
#include <stop_token>
#include <string_view>
#include <utility>

#include "sub0pipeline/error.hpp"

namespace sub0pipeline
{

namespace detail
{
struct DeferredSlot;
} // namespace detail

/**
 * Finishes one run of a deferred job, from any thread.
 *
 * Pipeline::emplaceDeferred() passes one to the job's start function. The job
 * stays kAwaiting, holding no executor thread, until the handle is completed;
 * only then are its dependants released. Move-only: exactly one holder can
 * complete a job, and completing empties the handle.
 *
 * Destroying a handle that still holds a job completes it with kJobFailed and
 * the message "completion abandoned". A completion that arrives after the job
 * was cancelled or timed out is ignored; until it arrives, the work counts as
 * an orphan (Pipeline::hasPendingOrphans(), Pipeline::joinOrphans()).
 *
 * @note All members are thread-safe with respect to the pipeline. One handle
 *       must not be used from two threads at once.
 */
class Completion
{
public:
    /** Creates an empty handle that holds no job. */
    constexpr Completion() noexcept = default;

    /** Takes the job from @p other, leaving it empty. */
    Completion(Completion&& other) noexcept : slot_{std::exchange(other.slot_, nullptr)} {}

    /**
     * Abandons the job this handle holds, if any, then takes the job from @p other.
     * @param other  The handle to take over; left empty.
     * @return *this.
     */
    Completion& operator=(Completion&& other) noexcept;

    /** Abandons the job this handle holds, if any. */
    ~Completion();

    Completion(const Completion&)            = delete;
    Completion& operator=(const Completion&) = delete;

    /** Completes the job successfully. No effect on an empty handle. */
    void succeed() noexcept { complete({}); }

    /**
     * Completes the job with an error.
     * @param error    The job's error; kCancelled and kTimeout map to their statuses.
     * @param message  Diagnostic text for IObserver::onJobFailure. Copied; a
     *                 non-empty message may allocate.
     */
    void fail(PipelineError error, std::string_view message = {}) noexcept
    {
        complete(std::unexpected(error), message);
    }

    /**
     * Completes the job with @p result and empties the handle.
     * @param result   Success, or the job's error.
     * @param message  Diagnostic text reported with an error. Copied.
     * @note No effect on an empty handle. Ignored, apart from releasing the
     *       orphan, when the job already finished as cancelled or timed out.
     */
    void complete(std::expected<void, PipelineError> result, std::string_view message = {}) noexcept;

    /**
     * Returns the job's stop token. Register a std::stop_callback on it to
     * abort the pending work on cancellation or timeout.
     * @return The token, or an empty token for an empty handle.
     */
    [[nodiscard]] std::stop_token stopToken() const noexcept;

    /**
     * Reports whether this handle still holds a job.
     * @return true until the handle is completed or moved from.
     */
    [[nodiscard]] bool valid() const noexcept { return slot_ != nullptr; }

private:
    friend struct detail::DeferredSlot;

    explicit Completion(detail::DeferredSlot* slot) noexcept : slot_{slot} {}

    detail::DeferredSlot* slot_{nullptr}; ///< non-owning; the pipeline owns the slot
};

} // namespace sub0pipeline
