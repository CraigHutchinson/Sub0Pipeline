// examples/tick_loop/main.cpp
//
// Demonstrates add_tick() and the run_loop() event loop.
//
// Structure:
//   1. Boot phase  — sequential 2-job pipeline (init → ready).
//   2. Tick phase  — three recurring jobs registered with add_tick():
//        "heartbeat"   every 200ms  — prints tick count
//        "sensor_poll" every 500ms  — prints sensor reading
//        "watchdog"    every 100ms  — silent counter
//   3. run_loop() is [[noreturn]], so it is launched on a std::thread.
//      The main thread sleeps for 1 100 ms then calls std::exit(0).
//
// DISABLED: not built by examples/CMakeLists.txt. run_loop() has no stop
// mechanism, so this example can only terminate by detaching a thread that
// borrows `pipeline` and calling std::exit. AGENTS.md forbids detaching work
// that can still access borrowed state. Re-enable once run_loop() can be
// stopped and joined.
//
// Expected tick counts over ~1 100 ms:
//   heartbeat   every 200 ms  →  ~5 ticks
//   sensor_poll every 500 ms  →  ~2 ticks
//   watchdog    every 100 ms  →  ~11 ticks

#include <sub0pipeline/sub0pipeline.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace sub0pipeline { std::unique_ptr<IExecutor> makeSequentialExecutor(); }

using namespace sub0pipeline;
using namespace std::chrono_literals;

int main()
{
    // ── Boot phase ────────────────────────────────────────────────────────────
    std::printf("=== Boot phase ===\n");

    Pipeline pipeline;
    auto exec = makeSequentialExecutor();

    auto init  = pipeline.emplace([] {
        std::printf("  [init]  subsystems initialised\n");
    }).name("init");

    auto ready = pipeline.emplace([] {
        std::printf("  [ready] system ready\n");
    }).name("ready");

    ready.succeed(init);

    auto bootResult = pipeline.run(*exec);
    if (!bootResult) {
        std::printf("Boot failed — aborting.\n");
        return 1;
    }
    std::printf("\n");

    // ── Tick registration ─────────────────────────────────────────────────────
    std::printf("=== Registering tick jobs ===\n");

    std::atomic<int> heartbeatCount{0};
    std::atomic<int> watchdogCount{0};

    pipeline.add_tick({
        .name     = "heartbeat",
        .interval = 200ms,
        .fn       = [&heartbeatCount] {
            const int n = heartbeatCount.fetch_add(1, std::memory_order_relaxed) + 1;
            std::printf("  [heartbeat] tick #%d\n", n);
        }
    });

    pipeline.add_tick({
        .name     = "sensor_poll",
        .interval = 500ms,
        .fn       = [] {
            std::printf("  [sensor_poll] reading sensor\n");
        }
    });

    pipeline.add_tick({
        .name     = "watchdog",
        .interval = 100ms,
        .fn       = [&watchdogCount] {
            watchdogCount.fetch_add(1, std::memory_order_relaxed);
            // Silent: just increments the counter.
        }
    });

    std::printf("  heartbeat   registered (every 200 ms, expected ~5 ticks)\n");
    std::printf("  sensor_poll registered (every 500 ms, expected ~2 ticks)\n");
    std::printf("  watchdog    registered (every 100 ms, expected ~11 ticks)\n");
    std::printf("\n");

    // ── Tick loop (background thread) ─────────────────────────────────────────
    // run_loop() never returns, so launch it on a detached thread.
    // After 1 100 ms on the main thread we call std::exit(0). This is the
    // reason the example is disabled — see the header comment.
    std::printf("=== Tick loop running for 1 100 ms ===\n");

    std::thread([&pipeline] {
        pipeline.run_loop();   // [[noreturn]]
    }).detach();

    std::this_thread::sleep_for(1100ms);

    std::printf("\n=== Shutdown ===\n");
    std::printf("  heartbeat   fired %d time(s)\n",
                heartbeatCount.load(std::memory_order_relaxed));
    std::printf("  watchdog    fired %d time(s)\n",
                watchdogCount.load(std::memory_order_relaxed));
    std::printf("  (std::exit — run_loop has no stop mechanism yet)\n");

    std::exit(0);
}
