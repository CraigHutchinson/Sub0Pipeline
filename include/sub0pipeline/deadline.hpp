#pragma once

#include <atomic>
#include <chrono>
#include <stop_token>
#include <utility>

namespace sub0pipeline {

/// Caller-owned registration. Only the registered service may expire it.
/// Expiry requests stop synchronously: call from task context, never an ISR.
class Deadline final {
public:
    explicit Deadline(std::stop_source source) noexcept : source_{std::move(source)} {}
    Deadline(const Deadline&) = delete;
    Deadline& operator=(const Deadline&) = delete;

    void expire() noexcept {
        expired_.store(true, std::memory_order_release);
        source_.request_stop();
    }
    [[nodiscard]] bool expired() const noexcept {
        return expired_.load(std::memory_order_acquire);
    }
private:
    std::stop_source source_;
    std::atomic<bool> expired_{false};
};

/// Optional platform clock/timer service; borrowed until all runs/triggers join.
/// arm() starts a relative execution deadline (queue time is excluded). It may
/// expire synchronously. false means capacity exhausted, with no registration.
/// cancel_and_wait() must remove the registration AND drain any expiry callback
/// before returning, even if it already fired. Both methods must be thread-safe,
/// non-throwing and task-context safe. Do not hold service locks while expiring:
/// request_stop() can synchronously run consumer callbacks.
/// A service can use fixed slots; registration itself requires no heap allocation.
class IDeadlineService {
public:
    virtual ~IDeadlineService() = default;
    virtual bool arm(Deadline&, std::chrono::milliseconds) noexcept = 0;
    virtual void cancel_and_wait(Deadline&) noexcept = 0;
};

} // namespace sub0pipeline
