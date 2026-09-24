# Bounded Zephyr execution

A one-worker task-context adapter with two fixed queue/task slots in the example,
a 16 KiB worker stack and caller-runs overflow. Jobs must be affinity-independent,
non-throwing and tolerate recursive inline successors; no synchronous sibling
waits. Stack/priority hints per job are ignored. Shutdown stops producers, drains
callbacks, reaps orphan workers and joins the Zephyr worker before storage dies.

Validated with Zephyr 4.1.0 `native_sim/native/64`, host GCC 13 and its full C++23
standard library, with C++ exceptions disabled. This is **not** validation of
hardware targets or Zephyr's minimal C++ library. The core still requires standard
mutex/stop-token support and native thread support for default timeout helpers.

```sh
export ZEPHYR_BASE=/path/to/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=host
cmake -S examples/zephyr_bounded -B build-zephyr -GNinja \
  -DBOARD=native_sim/native/64
cmake --build build-zephyr
build-zephyr/zephyr/zephyr.exe -stop_at=2
```

The executable must print `Sub0Pipeline Zephyr bounded: PASS`. It checks queue
saturation/fan-in, stop-aware semaphore I/O, ACK suppression and cancellation of
an in-flight on-demand job followed by owner shutdown. CI checks that marker.

Fixed queue slots do not cover std::function, graph, traversal or stop-state
allocations. Treat allocator exhaustion as a separate system budget. Scheduler
calls and stop requests remain task-context-only. An ISR may hand off bounded
stable event IDs via `k_msgq_put(..., K_NO_WAIT)` to a task; it must not call this
executor or expire a deadline. Define overflow and producer shutdown before
adding that integration.

References: [message queues](https://docs.zephyrproject.org/latest/kernel/services/data_passing/message_queues.html)
and [thread lifecycle](https://docs.zephyrproject.org/latest/kernel/services/threads/index.html).
