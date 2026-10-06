#define ANKERL_NANOBENCH_IMPLEMENT
#include "nanobench.h"
#include "sub0pipeline/sub0pipeline.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace
{

void printSystemInfo()
{
    printf("== System Info ==\n");

#ifdef _WIN32
    printf("OS: Windows\n");
#elif defined(__APPLE__)
    printf("OS: macOS\n");
#elif defined(__linux__)
    printf("OS: Linux\n");
#endif

#if defined(_MSC_VER)
    printf("Compiler: MSVC %d\n", _MSC_VER);
#elif defined(__clang__)
    printf("Compiler: Clang %d.%d.%d\n", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    printf("Compiler: GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif

#ifdef _WIN32
    {
        char cpuName[256] = "Unknown";
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            DWORD size = sizeof(cpuName);
            RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(cpuName), &size);
            RegCloseKey(hKey);
        }
        printf("CPU: %s\n", cpuName);
    }
#elif defined(__linux__)
    {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string   line;
        while (std::getline(cpuinfo, line))
        {
            if (line.find("model name") != std::string::npos)
            {
                auto pos = line.find(':');
                if (pos != std::string::npos)
                    printf("CPU:%s\n", line.substr(pos + 1).c_str());
                break;
            }
        }
    }
#elif defined(__APPLE__)
    {
        char   buf[256];
        size_t len = sizeof(buf);
        if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0)
            printf("CPU: %s\n", buf);
    }
#endif

    printf("Threads: %u\n", std::thread::hardware_concurrency());

#ifdef NDEBUG
    printf("Build: Release\n");
#else
    printf("Build: Debug\n");
#endif

    printf("==\n\n");
}

// ── Inline executor (zero overhead) ──────────────────────────────────────────

class InlineExecutor final : public sub0pipeline::IExecutor
{
public:
    void dispatch(std::string_view, std::function<void()> fn,
                  std::function<void()> oc, int, uint8_t, uint32_t) override
    {
        fn();
        if (oc) oc();
    }
    void waitAll() override {}
    [[nodiscard]] int concurrency() const noexcept override { return 1; }
};

// ── Graph shapes ─────────────────────────────────────────────────────────────

using sub0pipeline::Job;
using sub0pipeline::Pipeline;

void buildChain(Pipeline& pipeline, int jobs)
{
    Job previous;
    for (int i = 0; i < jobs; ++i)
    {
        auto job = pipeline.emplace([] {}).name("j" + std::to_string(i));
        if (previous.valid()) job.succeed(previous);
        previous = job;
    }
}

void buildFanOut(Pipeline& pipeline, int leaves)
{
    auto root = pipeline.emplace([] {}).name("root");
    for (int i = 0; i < leaves; ++i)
        pipeline.emplace([] {}).name("leaf_" + std::to_string(i)).succeed(root);
}

// `layers` rows of `width` jobs; each job depends on `fanIn` jobs of the row above.
void buildLayered(Pipeline& pipeline, int layers, int width, int fanIn)
{
    std::vector<Job> above, row;
    for (int layer = 0; layer < layers; ++layer)
    {
        row.clear();
        for (int i = 0; i < width; ++i)
        {
            auto job = pipeline.emplace([] {});
            for (int k = 0; k < fanIn && !above.empty(); ++k)
                job.succeed(above[static_cast<std::size_t>((i + k) % width)]);
            row.push_back(job);
        }
        above = row;
    }
}

// Older baselines built with this harness have no Pipeline::reserve.
template<typename P>
constexpr bool kHasReserve = requires(P& pipeline) { pipeline.reserve(std::size_t{1}); };

template<typename P>
void reserveIfSupported(P& pipeline, std::size_t jobs)
{
    if constexpr (kHasReserve<P>) pipeline.reserve(jobs);
}

// ── Case runner ──────────────────────────────────────────────────────────────

/// How long one operation takes, which sets how it is sampled.
enum class Cost
{
    kCheap,     ///< Sub-microsecond: 1,000 warmup, >= 100,000 iterations per epoch.
    kMedium,    ///< Tens of microseconds: 10 warmup, >= 1,000 iterations per epoch.
    kThreaded,  ///< Creates or wakes native threads: 2 warmup, >= 10 iterations per epoch.
};

/// Runs each selected case under nanobench, or, for profilers, in a plain loop
/// for a fixed wall time so that every sample lands in one workload.
class Runner
{
public:
    enum class Mode { kBench, kList, kProfile };

    Mode mode{Mode::kBench};
    std::string filter;         ///< Empty selects every case; else a name substring.
    bool exact{false};          ///< Match `filter` against the whole name instead.
    double profileSeconds{0.0}; ///< Loop time per case in kProfile mode.
    std::vector<ankerl::nanobench::Result> results;

    void group(const char* title, Cost cost)
    {
        collect();
        cost_ = cost;
        bench_.title(title);
        switch (cost)
        {
            case Cost::kCheap:    bench_.warmup(1'000).minEpochIterations(100'000); break;
            case Cost::kMedium:   bench_.warmup(10).minEpochIterations(1'000); break;
            case Cost::kThreaded: bench_.warmup(2).minEpochIterations(10); break;
        }
    }

    [[nodiscard]] bool selected(std::string_view name) const
    {
        if (filter.empty()) return true;
        return exact ? name == filter : name.find(filter) != std::string_view::npos;
    }

    template<typename Body>
    void run(const char* name, Body&& body)
    {
        if (!selected(name)) return;
        switch (mode)
        {
            case Mode::kList:    std::printf("%s\n", name); break;
            case Mode::kBench:   bench_.run(name, body); break;
            case Mode::kProfile: loop(name, body); break;
        }
    }

    void collect()
    {
        const auto& section = bench_.results();
        results.insert(results.end(), section.begin(), section.end());
    }

private:
    template<typename Body>
    void loop(const char* name, Body& body)
    {
        using Clock = std::chrono::steady_clock;
        const int batch = cost_ == Cost::kCheap ? 256 : 1;
        const auto start = Clock::now();
        const auto deadline = start + std::chrono::duration<double>{profileSeconds};
        std::uint64_t iterations = 0;
        do
        {
            for (int i = 0; i < batch; ++i) body();
            iterations += static_cast<std::uint64_t>(batch);
        } while (Clock::now() < deadline);
        const std::chrono::duration<double> elapsed = Clock::now() - start;
        std::printf("profile,\"%s\",%llu,%.3f,%.1f\n", name,
                    static_cast<unsigned long long>(iterations), elapsed.count(),
                    elapsed.count() * 1e9 / static_cast<double>(iterations));
    }

    ankerl::nanobench::Bench bench_;
    Cost cost_{Cost::kCheap};
};

int usage(const char* program)
{
    std::fprintf(stderr,
        "Usage: %s [--json PATH] [--features] [--case SUBSTR] [--exact] [--list]\n"
        "          [--profile-seconds N]\n"
        "  --case SUBSTR         run only cases whose name contains SUBSTR\n"
        "  --exact               require --case to equal the whole case name\n"
        "  --list                print the selected case names and exit\n"
        "  --profile-seconds N   loop each selected case for N seconds without\n"
        "                        nanobench, for use under a sampling profiler\n",
        program);
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace sub0pipeline;

    Runner runner;
    std::string output;
    bool features = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument{argv[i]};
        if (argument == "--features") features = true;
        else if (argument == "--list") runner.mode = Runner::Mode::kList;
        else if (argument == "--exact") runner.exact = true;
        else if (argument == "--json" && i + 1 < argc) output = argv[++i];
        else if (argument == "--case" && i + 1 < argc) runner.filter = argv[++i];
        else if (argument == "--profile-seconds" && i + 1 < argc)
        {
            runner.mode = Runner::Mode::kProfile;
            runner.profileSeconds = std::atof(argv[++i]);
            if (runner.profileSeconds <= 0.0) return usage(argv[0]);
        }
        else return usage(argv[0]);
    }
    // Unbuffered: if a case hangs, the capture script can show the last one that finished.
    std::cout.setf(std::ios::unitbuf);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (runner.mode == Runner::Mode::kBench) printSystemInfo();

    InlineExecutor exec;

    // ── DAG construction ──────────────────────────────────────────────────────

    runner.group("DAG construction", Cost::kCheap);

    runner.run("construct 10-job linear chain", []
    {
        Pipeline pipeline;
        buildChain(pipeline, 10);
        ankerl::nanobench::doNotOptimizeAway(&pipeline);
    });

    runner.run("construct 10-job fan-out (1 root + 9 leaves)", []
    {
        Pipeline pipeline;
        buildFanOut(pipeline, 9);
        ankerl::nanobench::doNotOptimizeAway(&pipeline);
    });

    // ── Sequential execution ──────────────────────────────────────────────────
    // Each case re-runs one built pipeline; run() resets its state internally.

    runner.group("Sequential execution (InlineExecutor)", Cost::kCheap);

    {
        Pipeline pipeline;
        buildChain(pipeline, 10);
        runner.run("10-job linear chain", [&] { (void)pipeline.run(exec); });
    }

    {
        Pipeline pipeline;
        buildFanOut(pipeline, 9);
        runner.run("10-job fan-out (1 root + 9 leaves)", [&] { (void)pipeline.run(exec); });
    }

    {
        Pipeline pipeline;
        std::vector<Job> leaves;
        for (int i = 0; i < 9; ++i)
            leaves.push_back(pipeline.emplace([] {}).name("leaf_" + std::to_string(i)));
        auto sink = pipeline.emplace([] {}).name("sink");
        for (auto& leaf : leaves) sink.succeed(leaf);

        runner.run("10-job fan-in (9 roots + 1 sink)", [&] { (void)pipeline.run(exec); });
    }

    {
        Pipeline pipeline;
        auto a = pipeline.emplace([] {}).name("A");
        auto b = pipeline.emplace([] {}).name("B");
        auto c = pipeline.emplace([] {}).name("C");
        auto d = pipeline.emplace([] {}).name("D");
        a.precede(b, c);
        d.succeed(b, c);

        runner.run("4-job diamond", [&] { (void)pipeline.run(exec); });
    }

    // ── Validation ────────────────────────────────────────────────────────────

    runner.group("Validation", Cost::kCheap);

    {
        Pipeline pipeline;
        buildChain(pipeline, 20);
        runner.run("validate 20-job chain", [&]
        {
            ankerl::nanobench::doNotOptimizeAway(pipeline.validate());
        });
    }

    // ── Library inline executors ─────────────────────────────────────────────
    // The cases above use this file's own executor. These use the two the
    // library ships, which is what runInline() and bare-metal callers get.

    runner.group("Library inline executors", Cost::kCheap);

    {
        SequentialExecutor sequential;
        Pipeline pipeline;
        buildChain(pipeline, 10);
        runner.run("run_inline: 10-job linear chain", [&] { (void)pipeline.runInline(); });
        runner.run("sequential executor: 10-job linear chain", [&]
        {
            (void)pipeline.run(sequential);
        });
    }

    // ── Scale ─────────────────────────────────────────────────────────────────
    // Larger graphs: per-job cost once the graph no longer fits in L1, and the
    // successor pool once fan-out exceeds the four inline slots. The chain is
    // kept to 200 jobs because an inline executor recurses once per link.

    runner.group("Scale (InlineExecutor)", Cost::kMedium);

    runner.run("construct 1000-job layered DAG (20x50, fan-in 4)", []
    {
        Pipeline pipeline;
        buildLayered(pipeline, 20, 50, 4);
        ankerl::nanobench::doNotOptimizeAway(&pipeline);
    });

    if (kHasReserve<Pipeline>)
    {
        runner.run("construct 1000-job layered DAG, reserved", []
        {
            Pipeline pipeline;
            reserveIfSupported(pipeline, 1000);
            buildLayered(pipeline, 20, 50, 4);
            ankerl::nanobench::doNotOptimizeAway(&pipeline);
        });
    }

    runner.run("construct 300-job fan-out (1 root + 299 leaves)", []
    {
        Pipeline pipeline;
        buildFanOut(pipeline, 299);
        ankerl::nanobench::doNotOptimizeAway(&pipeline);
    });

    {
        Pipeline pipeline;
        buildChain(pipeline, 200);
        runner.run("200-job linear chain", [&] { (void)pipeline.run(exec); });
    }

    {
        Pipeline pipeline;
        buildFanOut(pipeline, 299);
        runner.run("300-job fan-out (1 root + 299 leaves)", [&] { (void)pipeline.run(exec); });
    }

    {
        Pipeline pipeline;
        buildLayered(pipeline, 20, 50, 4);
        runner.run("1000-job layered DAG (20x50, fan-in 4)", [&] { (void)pipeline.run(exec); });
        runner.run("run_inline: 1000-job layered DAG", [&] { (void)pipeline.runInline(); });
        runner.run("validate 1000-job layered DAG", [&]
        {
            ankerl::nanobench::doNotOptimizeAway(pipeline.validate());
        });
        runner.run("snapshot 1000-job layered DAG", [&]
        {
            ankerl::nanobench::doNotOptimizeAway(pipeline.snapshot());
        });
    }

    // ── Threaded executors ────────────────────────────────────────────────────
    // No-op jobs, so these measure dispatch, wake-up and contention only. The
    // pool is fixed at four workers to keep results comparable across hosts.

    runner.group("Threaded executors (no-op jobs)", Cost::kThreaded);

    {
        DesktopExecutor desktop;
        Pipeline pipeline;
        buildFanOut(pipeline, 9);
        runner.run("desktop: 10-job fan-out", [&] { (void)pipeline.run(desktop); });
    }

    {
        PriorityExecutor pool{{.threadCount = 4}};

        Pipeline fanOut;
        buildFanOut(fanOut, 9);
        runner.run("priority(4): 10-job fan-out", [&] { (void)fanOut.run(pool); });

        Pipeline chain;
        buildChain(chain, 10);
        runner.run("priority(4): 10-job linear chain", [&] { (void)chain.run(pool); });
        runner.run("scoped over priority(4): 10-job linear chain", [&]
        {
            ScopedExecutor scoped{pool};
            (void)chain.run(scoped);
        });

        Pipeline wide;
        buildFanOut(wide, 299);
        runner.run("priority(4): 300-job fan-out", [&] { (void)wide.run(pool); });

        Pipeline layered;
        buildLayered(layered, 20, 50, 4);
        runner.run("priority(4): 1000-job layered DAG (20x50, fan-in 4)", [&]
        {
            (void)layered.run(pool);
        });

        Pipeline onDemand;
        auto job = onDemand.addOnDemand([]() -> std::expected<void, PipelineError> { return {}; });
        onDemand.arm(pool);
        runner.run("priority(4): on-demand trigger and wait", [&]
        {
            (void)onDemand.trigger(job);
            pool.waitAll();
        });
    }

    if (features)
    {
        struct Observer final : IObserver
        {
            std::size_t calls = 0;
            void onJobStart(RunId, JobId, std::string_view) override { ++calls; }
            void onJobFinish(RunId, JobId, std::string_view, JobStatus, float) override { ++calls; }
        } observer;
        Pipeline pipeline;
        Job previous;
        for (int i = 0; i < 10; ++i)
        {
            auto job = pipeline.emplace([] {});
            if (previous.valid()) job.succeed(previous);
            previous = job;
        }
        std::stop_source stop;
        runner.group("Opt-in features (10-job chain)", Cost::kCheap);
        runner.run("external stoppable token, no request", [&] {
            (void)pipeline.run(exec, stop.get_token());
        });
        runner.run("observer callbacks, no external token", [&] {
            (void)pipeline.run(exec, &observer);
            ankerl::nanobench::doNotOptimizeAway(observer.calls);
        });

        // Helper-thread startup dominates these opt-in timeout measurements.
        // Keep sampling separate from the inexpensive DAG benchmarks.
        runner.group("Opt-in timeout helpers (one immediate job)", Cost::kThreaded);
        Pipeline cooperative;
        (void)cooperative.emplace([](std::stop_token) -> std::expected<void, PipelineError> {
            return {};
        }).timeout(std::chrono::milliseconds{10});
        runner.run("cooperative timeout configured", [&] {
            (void)cooperative.run(exec);
        });
        Pipeline plain;
        (void)plain.emplace([] {}).timeout(std::chrono::milliseconds{10});
        runner.run("plain timeout configured plus join", [&] {
            (void)plain.run(exec);
            plain.joinOrphans();
        });
    }

    runner.collect();

    if (!output.empty() && runner.mode == Runner::Mode::kBench)
    {
        std::ofstream file{output};
        ankerl::nanobench::render(ankerl::nanobench::templates::json(), runner.results, file);
        if (!file)
        {
            std::fprintf(stderr, "Cannot write benchmark JSON: %s\n", output.c_str());
            return 1;
        }
    }

    return 0;
}
