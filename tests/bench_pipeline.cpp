#define ANKERL_NANOBENCH_IMPLEMENT
#include "nanobench.h"
#include <sub0pipeline/sub0pipeline.hpp>

#include <cstdio>
#include <fstream>
#include <string_view>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace {

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
        while (std::getline(cpuinfo, line)) {
            if (line.find("model name") != std::string::npos) {
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
    void wait_all() override {}
    [[nodiscard]] int concurrency() const noexcept override { return 1; }
};

} // namespace

int main(int argc, char** argv)
{
    std::string output;
    bool features = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--features") features = true;
        else if (argument == "--json" && i + 1 < argc) output = argv[++i];
        else {
            std::fprintf(stderr, "Usage: %s [--json PATH] [--features]\n", argv[0]);
            return 2;
        }
    }
    printSystemInfo();

    ankerl::nanobench::Bench bench;
    bench.warmup(1'000).minEpochIterations(100'000);
    std::vector<ankerl::nanobench::Result> results;
    const auto capture = [&] {
        const auto& section = bench.results();
        results.insert(results.end(), section.begin(), section.end());
    };

    InlineExecutor exec;

    // ── DAG construction ──────────────────────────────────────────────────────

    bench.title("DAG construction");

    bench.run("construct 10-job linear chain", []
    {
        sub0pipeline::Pipeline pipeline;
        sub0pipeline::Job      prev;
        for (int i = 0; i < 10; ++i) {
            auto j = pipeline.emplace([] {}).name("j" + std::to_string(i));
            if (prev.valid()) j.succeed(prev);
            prev = j;
        }
        ankerl::nanobench::doNotOptimizeAway(&pipeline);
    });

    bench.run("construct 10-job fan-out (1 root + 9 leaves)", []
    {
        sub0pipeline::Pipeline pipeline;
        auto root = pipeline.emplace([] {}).name("root");
        for (int i = 0; i < 9; ++i) {
            pipeline.emplace([] {}).name("leaf_" + std::to_string(i)).succeed(root);
        }
        ankerl::nanobench::doNotOptimizeAway(&pipeline);
    });

    // ── Sequential execution ──────────────────────────────────────────────────

    capture();
    bench.title("Sequential execution (InlineExecutor)");

    {
        sub0pipeline::Pipeline pipeline;
        sub0pipeline::Job      prev;
        for (int i = 0; i < 10; ++i) {
            auto j = pipeline.emplace([] {}).name("j" + std::to_string(i));
            if (prev.valid()) j.succeed(prev);
            prev = j;
        }
        bench.run("10-job linear chain", [&]
        {
            // Re-run the same pipeline each iteration (resets state internally).
            (void)pipeline.run(exec);
        });
    }

    {
        sub0pipeline::Pipeline pipeline;
        auto root = pipeline.emplace([] {}).name("root");
        for (int i = 0; i < 9; ++i) {
            pipeline.emplace([] {}).name("leaf_" + std::to_string(i)).succeed(root);
        }
        bench.run("10-job fan-out (1 root + 9 leaves)", [&]
        {
            (void)pipeline.run(exec);
        });
    }

    {
        sub0pipeline::Pipeline pipeline;
        std::vector<sub0pipeline::Job> leaves;
        for (int i = 0; i < 9; ++i) {
            leaves.push_back(pipeline.emplace([] {}).name("leaf_" + std::to_string(i)));
        }
        auto sink = pipeline.emplace([] {}).name("sink");
        for (auto& leaf : leaves) sink.succeed(leaf);

        bench.run("10-job fan-in (9 roots + 1 sink)", [&]
        {
            (void)pipeline.run(exec);
        });
    }

    {
        sub0pipeline::Pipeline pipeline;
        auto a = pipeline.emplace([] {}).name("A");
        auto b = pipeline.emplace([] {}).name("B");
        auto c = pipeline.emplace([] {}).name("C");
        auto d = pipeline.emplace([] {}).name("D");
        a.precede(b, c);
        d.succeed(b, c);

        bench.run("4-job diamond", [&]
        {
            (void)pipeline.run(exec);
        });
    }

    // ── Validation ────────────────────────────────────────────────────────────

    capture();
    bench.title("Validation");

    {
        sub0pipeline::Pipeline pipeline;
        sub0pipeline::Job      prev;
        for (int i = 0; i < 20; ++i) {
            auto j = pipeline.emplace([] {}).name("j" + std::to_string(i));
            if (prev.valid()) j.succeed(prev);
            prev = j;
        }
        bench.run("validate 20-job chain", [&]
        {
            ankerl::nanobench::doNotOptimizeAway(pipeline.validate());
        });
    }

    capture();

    if (features) {
        using namespace sub0pipeline;
        struct Observer final : IObserver {
            std::size_t calls = 0;
            void onStart(std::string_view) override { ++calls; }
            void onFinish(std::string_view, JobStatus, float) override { ++calls; }
        } observer;
        Pipeline pipeline;
        Job previous;
        for (int i = 0; i < 10; ++i) {
            auto job = pipeline.emplace([] {});
            if (previous.valid()) job.succeed(previous);
            previous = job;
        }
        std::stop_source stop;
        bench.title("Opt-in features (10-job chain)");
        bench.run("external stoppable token, no request", [&] {
            (void)pipeline.run(exec, stop.get_token());
        });
        bench.run("observer callbacks, no external token", [&] {
            (void)pipeline.run(exec, &observer);
            ankerl::nanobench::doNotOptimizeAway(observer.calls);
        });
        capture();

        // Helper-thread startup dominates these opt-in timeout measurements.
        // Keep sampling separate from the inexpensive DAG benchmarks.
        bench.title("Opt-in timeout helpers (one immediate job)");
        bench.warmup(2).minEpochIterations(10);
        Pipeline cooperative;
        (void)cooperative.emplace([](std::stop_token) -> std::expected<void, PipelineError> {
            return {};
        }).timeout(std::chrono::milliseconds{10});
        bench.run("cooperative timeout configured", [&] {
            (void)cooperative.run(exec);
        });
        Pipeline plain;
        (void)plain.emplace([] {}).timeout(std::chrono::milliseconds{10});
        bench.run("plain timeout configured plus join", [&] {
            (void)plain.run(exec);
            plain.join_orphans();
        });
        capture();
    }

    if (!output.empty()) {
        std::ofstream file{output};
        ankerl::nanobench::render(ankerl::nanobench::templates::json(), results, file);
        if (!file) {
            std::fprintf(stderr, "Cannot write benchmark JSON: %s\n", output.c_str());
            return 1;
        }
    }

    return 0;
}
