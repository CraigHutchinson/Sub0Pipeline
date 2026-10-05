// examples/observer_profiling/main.cpp
//
// Demonstrates a custom IObserver that renders a live boot-screen progress
// bar in the terminal, records per-job timing, and prints a Gantt-style
// completion summary.
//
// Pipeline topology:
//   nvs(20ms) → wifi(80ms)
//   nvs(20ms) → display(60ms)
//   wifi(80ms) → mqtt(40ms)
//   display(60ms) → ui(30ms)
//   mqtt + ui → app(10ms)

#include <sub0pipeline/sub0pipeline.hpp>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <cstdio>

namespace sub0pipeline { std::unique_ptr<IExecutor> makeDesktopExecutor(); }

using namespace sub0pipeline;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// ── BootObserver ──────────────────────────────────────────────────────────────

class BootObserver : public IObserver
{
public:
    void onJobStart(RunId, JobId jobId, std::string_view jobName) override
    {
        const std::string key(jobName);
        std::scoped_lock lock{mutex_};
        startTime_[jobId] = Clock::now();

        // Pad job name to a fixed width for alignment.
        char padded[20]{};
        std::snprintf(padded, sizeof(padded), "%-14s", key.c_str());
        std::printf("  [ ... ] %s\n", padded);
    }

    void onJobFinish(RunId, JobId jobId, std::string_view jobName,
                     JobStatus status, float progress) override
    {
        const std::string key(jobName);
        std::scoped_lock lock{mutex_};
        long long elapsedMs = 0;

        auto it = startTime_.find(jobId);
        if (it != startTime_.end()) {
            elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - it->second).count();
        }

        timings_[jobId] = { key, elapsedMs, status };

        // Build a filled/empty progress bar (10 blocks wide).
        constexpr int kBarWidth = 10;
        const int filled = static_cast<int>(progress * kBarWidth + 0.5f);
        char bar[64]{};
        int pos = 0;
        bar[pos++] = '[';
        for (int i = 0; i < kBarWidth; ++i) {
            // UTF-8 for U+2588 (FULL BLOCK) is 3 bytes; U+2591 (LIGHT SHADE) is 3 bytes.
            if (i < filled) {
                bar[pos++] = '\xe2'; bar[pos++] = '\x96'; bar[pos++] = '\x88'; // █
            } else {
                bar[pos++] = '\xe2'; bar[pos++] = '\x96'; bar[pos++] = '\x91'; // ░
            }
        }
        bar[pos++] = ']';
        bar[pos]   = '\0';

        const char* statusStr = (status == JobStatus::kDone)    ? "DONE"
                              : (status == JobStatus::kFailed)   ? "FAIL"
                              : (status == JobStatus::kSkipped)  ? "SKIP"
                              : (status == JobStatus::kTimedOut) ? "TIME"
                              :                                    "????";

        char padded[20]{};
        std::snprintf(padded, sizeof(padded), "%-14s", key.c_str());
        std::printf("  %s %3d%% %s  %4lldms  %s\n",
                    bar,
                    static_cast<int>(progress * 100.0f + 0.5f),
                    padded,
                    elapsedMs,
                    statusStr);
    }

    void onDependenciesResolved(RunId, JobId, std::string_view from,
                                DependencyRange successors) override
    {
        std::scoped_lock lock{mutex_};
        for (const auto target : successors) {
            std::printf("  edge: %.*s \xe2\x86\x92 %.*s\n",
                        static_cast<int>(from.size()), from.data(),
                        static_cast<int>(target.name.size()), target.name.data());
        }
    }

    void printSummary() const
    {
        std::scoped_lock lock{mutex_};
        std::printf("\n--- Gantt summary (by start order) ---\n");
        std::printf("  %-14s  %8s  %s\n", "job", "ms", "status");
        std::printf("  %-14s  %8s  %s\n", "---", "--", "------");

        for (const auto& [jobId, rec] : timings_) {
            (void)jobId;
            const char* statusStr =
                (rec.status == JobStatus::kDone)    ? "kDone"
              : (rec.status == JobStatus::kFailed)   ? "kFailed"
              : (rec.status == JobStatus::kSkipped)  ? "kSkipped"
              : (rec.status == JobStatus::kTimedOut) ? "kTimedOut"
              :                                        "unknown";
            std::printf("  %-14s  %6lldms  %s\n",
                        rec.name.c_str(), rec.elapsedMs, statusStr);
        }
    }

private:
    struct TimingRecord {
        std::string name;
        long long elapsedMs{};
        JobStatus status{JobStatus::kPending};
    };

    mutable std::mutex mutex_;
    std::map<JobId, Clock::time_point> startTime_;
    std::map<JobId, TimingRecord>      timings_;
};

// ── Subsystem initialisers ────────────────────────────────────────────────────

namespace {

auto nvs_init() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(20ms);
    return {};
}

auto wifi_init() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(80ms);
    return {};
}

auto display_init() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(60ms);
    return {};
}

auto mqtt_init() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(40ms);
    return {};
}

auto ui_init() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(30ms);
    return {};
}

auto app_start() -> std::expected<void, PipelineError>
{
    std::this_thread::sleep_for(10ms);
    return {};
}

} // namespace

// ── Main ──────────────────────────────────────────────────────────────────────

int main()
{
    Pipeline boot;

    auto nvs     = boot.emplace(nvs_init).name("nvs");
    auto wifi    = boot.emplace(wifi_init).name("wifi").timeout(500ms);
    auto display = boot.emplace(display_init).name("display").timeout(500ms);
    auto mqtt    = boot.emplace(mqtt_init).name("mqtt").timeout(500ms);
    auto ui      = boot.emplace(ui_init).name("ui");
    auto app     = boot.emplace(app_start).name("app");

    wifi.succeed(nvs);
    display.succeed(nvs);
    mqtt.succeed(wifi);
    ui.succeed(display);
    app.succeed(mqtt, ui);

    BootObserver observer;
    auto exec = makeDesktopExecutor();

    std::printf("Boot sequence starting...\n\n");
    const auto t0 = Clock::now();

    auto result = boot.run(*exec, &observer);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - t0);

    std::printf("\n");
    if (result) {
        std::printf("Boot complete in %lld ms\n",
                    static_cast<long long>(elapsed.count()));
    } else {
        std::printf("Boot failed.\n");
    }

    observer.printSummary();
    return 0;
}
