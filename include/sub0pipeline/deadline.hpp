#pragma once

#include <atomic>
#include <chrono>
#include <stop_token>
#include <utility>

namespace sub0pipeline
{

/**
 * Caller-owned registration. Only the registered service may expire it.
 * Expiry requests stop synchronously: call from task context, never an ISR.
 */
class Deadline final
{
public:
    /**
     * Creates a registration that stops @p source when it expires.
     * @param source  The stop source that expire() requests stop on.
     */
    explicit Deadline(std::stop_source source) noexcept : source_{std::move(source)} {}
    Deadline(const Deadline&) = delete;
    Deadline& operator=(const Deadline&) = delete;

    /**
     * Marks the registration expired and requests stop on its stop source.
     * @note Called by the registered service. Task context only; the stop
     *       request runs consumer callbacks synchronously.
     */
    void expire() noexcept
    {
        expired_.store(true, std::memory_order_release);
        source_.request_stop();
    }

    /**
     * Reports whether expire() has been called.
     * @return true once the service has expired this registration.
     * @note Thread-safe.
     */
    [[nodiscard]] bool expired() const noexcept
    {
        return expired_.load(std::memory_order_acquire);
    }
private:
    std::stop_source source_;
    std::atomic<bool> expired_{false};
};

/**
 * Optional platform clock/timer service; borrowed until all runs/triggers join.
 * arm() starts a relative execution deadline (queue time is excluded). It may
 * expire synchronously. false means capacity exhausted, with no registration.
 * cancelAndWait() must remove the registration AND drain any expiry callback
 * before returning, even if it already fired. Both methods must be thread-safe,
 * non-throwing and task-context safe. Do not hold service locks while expiring:
 * request_stop() can synchronously run consumer callbacks.
 * A service can use fixed slots; registration itself requires no heap allocation.
 */
class IDeadlineService
{
public:
    virtual ~IDeadlineService() = default;

    /**
     * Starts a relative execution deadline for @p deadline.
     * @param deadline  The caller-owned registration to expire. It must stay
     *                  alive until cancelAndWait() returns for it.
     * @param delay     Time from now until the registration expires.
     * @return true if the deadline is registered; false if the service has no
     *         free capacity, in which case nothing is registered.
     * @note Thread-safe, non-throwing, task context only.
     */
    [[nodiscard]] virtual bool arm(Deadline& deadline, std::chrono::milliseconds delay) noexcept = 0;

    /**
     * Removes the registration for @p deadline and drains any expiry callback.
     * Returns only when no callback for it is still running, even if it
     * already fired.
     * @param deadline  A registration previously passed to arm().
     * @note Thread-safe, non-throwing, task context only.
     */
    virtual void cancelAndWait(Deadline& deadline) noexcept = 0;
};

} // namespace sub0pipeline
