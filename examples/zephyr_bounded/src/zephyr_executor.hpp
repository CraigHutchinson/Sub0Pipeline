#pragma once
#include <sub0pipeline/sub0pipeline.hpp>
#include <zephyr/kernel.h>
#include <array>

// Fixed queue and one Zephyr worker. Saturation runs on the caller; therefore
// jobs must be thread-affinity independent and tolerate inline recursion.
// Task context only, serialized shutdown after producers stop. Callables and
// Pipeline state can still allocate; fixed queue storage is not heap-free run.
template<std::size_t Capacity = 8>
class ZephyrExecutor final : public sub0pipeline::IExecutor {
    static_assert(Capacity > 0);
public:
    ZephyrExecutor() {
        k_mutex_init(&mutex_);
        k_condvar_init(&ready_);
        k_msgq_init(&queue_, queueBuffer_.data(), sizeof(Task*), Capacity);
        k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_),
                        entry, this, nullptr, nullptr, 5, 0, K_NO_WAIT);
    }
    ~ZephyrExecutor() override {
        wait_all();
        Task* stop = nullptr;
        k_msgq_put(&queue_, &stop, K_FOREVER);
        k_thread_join(&thread_, K_FOREVER);
    }
    void dispatch(std::string_view, std::function<void()> fn,
                  std::function<void()> complete, int, uint8_t, uint32_t) override {
        k_mutex_lock(&mutex_, K_FOREVER);
        ++pending_;
        Task* slot = nullptr;
        for (auto& task : tasks_) if (!task.used) { slot = &task; task.used = true; break; }
        if (slot) { slot->fn = std::move(fn); slot->complete = std::move(complete); }
        k_mutex_unlock(&mutex_);
        if (slot) {
            // Each queued/running task owns a slot, so the queue cannot be full.
            if (k_msgq_put(&queue_, &slot, K_NO_WAIT) != 0) std::terminate();
        } else {
            fn();
            if (complete) complete();
            fn = {}; complete = {};
            finish(nullptr);
        }
    }
    void wait_all() override {
        k_mutex_lock(&mutex_, K_FOREVER);
        while (pending_ != 0) k_condvar_wait(&ready_, &mutex_, K_FOREVER);
        k_mutex_unlock(&mutex_);
    }
    int concurrency() const noexcept override { return 1; }
private:
    struct Task { std::function<void()> fn, complete; bool used = false; };
    static void entry(void* self, void*, void*) { static_cast<ZephyrExecutor*>(self)->loop(); }
    void loop() {
        for (;;) {
            Task* task;
            k_msgq_get(&queue_, &task, K_FOREVER);
            if (!task) return;
            task->fn();
            if (task->complete) task->complete();
            task->fn = {}; task->complete = {};
            finish(task);
        }
    }
    void finish(Task* task) {
        k_mutex_lock(&mutex_, K_FOREVER);
        if (task) task->used = false;
        --pending_;
        k_condvar_broadcast(&ready_);
        k_mutex_unlock(&mutex_);
    }
    std::array<Task, Capacity> tasks_{};
    alignas(Task*) std::array<char, Capacity * sizeof(Task*)> queueBuffer_{};
    k_msgq queue_{};
    k_mutex mutex_{};
    k_condvar ready_{};
    std::size_t pending_ = 0;
    k_thread thread_{};
    K_KERNEL_STACK_MEMBER(stack_, 16384);
};
