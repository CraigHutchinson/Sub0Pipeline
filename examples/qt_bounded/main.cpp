#include "qt_executor.hpp"
#include <sub0pipeline/run_scope.hpp>
#include <QCoreApplication>
#include <atomic>
#include <condition_variable>
#include <latch>

using namespace sub0pipeline;
int main(int argc, char** argv) {
    QCoreApplication app{argc, argv};
    QtExecutor executor{1};
    Pipeline pipe;
    std::atomic<int> calls{0};
    auto root = pipe.emplace([] {});
    auto sink = pipe.emplace([&] { ++calls; });
    for (int i = 0; i < 32; ++i) {
        auto job = pipe.emplace([&] { ++calls; });
        job.succeed(root).precede(sink);
    }
    if (!pipe.run(executor) || calls != 33) return 1;
    // Saturation must make progress even when successors dispatch on a worker.
    Pipeline io;
    std::latch entered{1};
    bool acknowledged = false;
    auto read = io.emplace([&](std::stop_token token) -> std::expected<void, PipelineError> {
        std::mutex mutex;
        std::condition_variable_any ready;
        std::unique_lock lock{mutex};
        entered.count_down();
        ready.wait(lock, token, [] { return false; });
        return std::unexpected(PipelineError::kCancelled);
    });
    auto ack = io.emplace([&] { acknowledged = true; }).succeed(read);
    {
        RunScope run{io, executor};
        entered.wait();
        run.request_stop();
        auto result = run.join();
        if (result || result.error() != PipelineError::kCancelled || !run.complete()) return 2;
    }
    return acknowledged || io.status(ack) != JobStatus::kSkipped ? 3 : 0;
}
