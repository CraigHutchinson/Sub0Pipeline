#include "zephyr_executor.hpp"
#include <zephyr/sys/printk.h>
#include <atomic>

using namespace sub0pipeline;
static int exercise() {
    ZephyrExecutor<2> executor;
    Pipeline pipe;
    std::atomic<int> calls{0};
    auto root = pipe.emplace([] {});
    auto ack = pipe.emplace([&] { ++calls; });
    for (int i = 0; i < 16; ++i) {
        auto task = pipe.emplace([&] { ++calls; });
        task.succeed(root).precede(ack);
    }
    if (!pipe.run(executor) || calls != 17) return 1;
    // Stop-aware device wait: registration wakes the Zephyr semaphore from
    // task context. The request comes from another job, without wall-clock sleep.
    Pipeline cancelled;
    std::stop_source stop;
    k_sem ready;
    k_sem_init(&ready, 0, 1);
    auto read = cancelled.emplace([&](std::stop_token token) -> std::expected<void, PipelineError> {
        std::stop_callback wake{token, [&] { k_sem_give(&ready); }};
        stop.request_stop();
        k_sem_take(&ready, K_FOREVER);
        return std::unexpected(PipelineError::kCancelled);
    });
    bool acknowledged = false;
    (void)cancelled.emplace([&] { acknowledged = true; }).succeed(read);
    auto result = cancelled.run(executor, stop.get_token());
    cancelled.join_orphans();
    if (result || result.error() != PipelineError::kCancelled || acknowledged) return 2;
    Pipeline io;
    k_sem entered;
    k_sem_init(&entered, 0, 1);
    auto event = io.add_on_demand([&](std::stop_token token) -> std::expected<void, PipelineError> {
        std::stop_callback wake{token, [&] { k_sem_give(&ready); }};
        k_sem_give(&entered);
        k_sem_take(&ready, K_FOREVER);
        return std::unexpected(PipelineError::kCancelled);
    });
    io.arm(executor);
    if (!io.trigger(event)) return 3;
    k_sem_take(&entered, K_FOREVER);
    event.cancel();
    executor.wait_all();
    io.join_orphans();
    if (io.status(event) != JobStatus::kCancelled) return 4;
    return 0;
}

int main() {
    if (const int error = exercise()) {
        printk("Sub0Pipeline Zephyr bounded: FAIL %d\n", error);
        return error;
    }
    // Report only after executor destruction has joined the worker.
    printk("Sub0Pipeline Zephyr bounded: PASS\n");
    return 0;
}
