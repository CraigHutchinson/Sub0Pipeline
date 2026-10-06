// examples/tick_loop/main.cpp
//
// Demonstrates TickLoop: recurring jobs after a start-up pipeline.
//
// Structure:
//   1. Boot phase  — sequential 2-job pipeline (init → ready).
//   2. Tick phase  — three recurring jobs added to a TickLoop:
//        "heartbeat"   every 200ms  — prints tick count
//        "sensor_poll" every 500ms  — prints sensor reading
//        "watchdog"    every 100ms  — silent counter
//   3. A std::jthread owns TickLoop::run(stop_token); shutdown requests stop and
//      joins the loop before the pipeline or tick state is destroyed.
//
// Expected tick counts over ~1 100 ms:
//   heartbeat   every 200 ms  →  ~5 ticks
//   sensor_poll every 500 ms  →  ~2 ticks
//   watchdog    every 100 ms  →  ~11 ticks

#include "sub0pipeline/sub0pipeline.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace sub0pipeline;
using namespace std::chrono_literals;

int main()
{
    // ── Boot phase ────────────────────────────────────────────────────────────
    std::printf("=== Boot phase ===\n");

    Pipeline pipeline;
    SequentialExecutor exec;

    auto init  = pipeline.emplace([] {
        std::printf("  [init]  subsystems initialised\n");
    }).name("init");

    auto ready = pipeline.emplace([] {
        std::printf("  [ready] system ready\n");
    }).name("ready");

    ready.succeed(init);

    auto bootResult = pipeline.run(exec);
    if (!bootResult) {
        std::printf("Boot failed — aborting.\n");
        return 1;
    }
    std::printf("\n");

    // ── Tick registration ─────────────────────────────────────────────────────
    std::printf("=== Registering tick jobs ===\n");

    std::atomic<int> heartbeatCount{0};
    std::atomic<int> watchdogCount{0};
    TickLoop ticks;

    ticks.add({   // heartbeat
        .interval = 200ms,
        .fn       = [&heartbeatCount] {
            const int n = heartbeatCount.fetch_add(1, std::memory_order_relaxed) + 1;
            std::printf("  [heartbeat] tick #%d\n", n);
        }
    });

    ticks.add({   // sensor_poll
        .interval = 500ms,
        .fn       = [] {
            std::printf("  [sensor_poll] reading sensor\n");
        }
    });

    ticks.add({   // watchdog
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
    std::printf("=== Tick loop running for 1 100 ms ===\n");

    std::jthread loop([&ticks](std::stop_token stop) {
        ticks.run(stop);
    });

    std::this_thread::sleep_for(1100ms);
    loop.request_stop();
    loop.join();

    std::printf("\n=== Shutdown ===\n");
    std::printf("  heartbeat   fired %d time(s)\n",
                heartbeatCount.load(std::memory_order_relaxed));
    std::printf("  watchdog    fired %d time(s)\n",
                watchdogCount.load(std::memory_order_relaxed));
}
