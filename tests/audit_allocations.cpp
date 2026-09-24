// Standalone opt-in audit. Never link these replacement operators into consumers.
#include <sub0pipeline/sub0pipeline.hpp>
#include <sub0pipeline/deadline.hpp>
#include <sub0pipeline/run_scope.hpp>
#include "test_helpers.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace audit {
std::atomic<bool> enabled{false};
std::atomic<std::size_t> calls{0}, bytes{0};
void record(std::size_t size) noexcept {
    if (enabled.load(std::memory_order_relaxed)) {
        calls.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(size, std::memory_order_relaxed);
    }
}
void* allocate(std::size_t size, std::size_t alignment = 0) {
    for (;;) {
        void* p = nullptr;
        if (!alignment) p = std::malloc(size ? size : 1);
#ifdef _WIN32
        else p = _aligned_malloc(size ? size : 1, alignment);
#else
        else if (posix_memalign(&p, alignment, size ? size : 1) != 0) p = nullptr;
#endif
        if (p) { record(size); return p; }
        if (auto handler = std::get_new_handler()) handler();
        else throw std::bad_alloc{};
    }
}
void releaseAligned(void* p) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
struct Counts { std::size_t calls, bytes; };
template<class F> Counts measure(F&& fn) {
    calls = 0; bytes = 0;
    struct Window {
        Window() { enabled = true; }
        ~Window() { enabled = false; }
    } window;
    fn(); // Must join all owned work before returning.
    return {calls.load(), bytes.load()};
}
}

void* operator new(std::size_t n) { return audit::allocate(n); }
void* operator new[](std::size_t n) { return audit::allocate(n); }
void* operator new(std::size_t n, std::align_val_t a) { return audit::allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return audit::allocate(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { audit::releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { audit::releaseAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { audit::releaseAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { audit::releaseAligned(p); }

using namespace sub0pipeline;
using namespace std::chrono_literals;

int main() {
    // Calibrate outside reported cases, including aligned allocation.
    auto calibration = audit::measure([] {
        auto* a = ::operator new(17);
        auto* b = ::operator new(64, std::align_val_t{64});
        ::operator delete(a);
        ::operator delete(b, std::align_val_t{64});
    });
    if (calibration.calls != 2 || calibration.bytes != 81) return 2;
    std::puts("case,operations,new_calls,requested_bytes");
    auto report = [](const char* name, int operations, auto&& fn) {
        const auto counts = audit::measure([&] { for (int i = 0; i < operations; ++i) fn(); });
        std::printf("%s,%d,%zu,%zu\n", name, operations, counts.calls, counts.bytes);
    };
    auto chain = [](Pipeline& pipe) {
        Job previous;
        for (int i = 0; i < 10; ++i) {
            auto job = pipe.emplace([] {});
            if (previous) job.succeed(previous);
            previous = job;
        }
    };
    auto check = [](auto result) { if (!result) std::abort(); };
    InlineExecutor executor;
    report("construct_destroy_chain10", 100, [&] { Pipeline pipe; chain(pipe); });
    Pipeline pipe;
    chain(pipe);
    report("first_run_chain10", 1, [&] { check(pipe.run(executor)); });
    report("warm_run_chain10", 100, [&] { check(pipe.run(executor)); });
    std::stop_source external;
    report("warm_external_token_chain10", 100, [&] { check(pipe.run(executor, external.get_token())); });
    report("explicit_validation_chain10", 100, [&] { check(pipe.validate()); });
    report("snapshot_chain10", 100, [&] { auto snapshot = pipe.snapshot(); if (snapshot.size() != 10) std::abort(); });

    Pipeline failed;
    auto root = failed.emplace([]() -> std::expected<void, PipelineError> {
        return std::unexpected(PipelineError::kJobFailed);
    });
    (void)failed.emplace([] {}).succeed(root);
    (void)failed.run(executor);
    report("warm_failure_skip", 100, [&] { if (failed.run(executor)) std::abort(); });

    // Single-thread, no-expiry fixture isolates registration overhead; this is
    // not a production timer service and does not model timer-driver allocations.
    struct NoExpiry final : IDeadlineService {
        bool arm(Deadline&, std::chrono::milliseconds) noexcept override { return true; }
        void cancel_and_wait(Deadline&) noexcept override {}
    } deadline;
    Pipeline cooperative;
    (void)cooperative.emplace([](std::stop_token) -> std::expected<void, PipelineError> { return {}; }).timeout(1h);
    cooperative.set_deadline_service(&deadline);
    check(cooperative.run(executor));
    report("injected_cooperative_deadline", 100, [&] { check(cooperative.run(executor)); });
    cooperative.set_deadline_service(nullptr);
    report("native_cooperative_deadline", 100, [&] { check(cooperative.run(executor)); });
    report("owned_run_scope_chain10", 100, [&] { RunScope scope{pipe, executor}; check(scope.join()); });

    Pipeline event;
    auto job = event.add_on_demand([]() -> std::expected<void, PipelineError> { return {}; });
    event.arm(executor);
    report("on_demand_retry", 100, [&] { check(event.trigger(job)); executor.wait_all(); event.join_orphans(); });
    return 0;
}
