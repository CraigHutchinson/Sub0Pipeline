# Sub0Pipeline — Platform Executor Roadmap

Executors implement the `IExecutor` interface and provide the execution backend
for the Pipeline DAG engine. Each executor is a separate CMake target so
consumers only link what they need.

---

## Shipped Executors

| Executor | Target | Status | Notes |
|---|---|---|---|
| `SequentialExecutor` | `Sub0Pipeline::Headless` | Done | Inline, no threads. Deterministic. Ideal for unit tests and bare-metal. |
| `DesktopExecutor` | `Sub0Pipeline::Desktop` | Done | One `std::thread` per dispatched job. `hardware_concurrency()` slots. |
| `FreeRtosExecutor` | ESP-IDF component | Done | `xTaskCreatePinnedToCore` per job. Dual-core ESP32-P4. Semaphore drain. |

---

## Planned Executors

### `StdAsyncExecutor`
- **Target:** `Sub0Pipeline::StdAsync`
- **Backend:** `std::async` + `std::future<void>`
- **Use case:** Portable multi-threading without manual thread management
- **Notes:** Limited parallelism on some platforms; future-chaining adds overhead

### `ThreadPoolExecutor`
- **Target:** `Sub0Pipeline::ThreadPool`
- **Backend:** Fixed-size worker thread pool with a lock-free job queue
- **Use case:** High-throughput pipelines where task-creation overhead matters
- **Notes:** Requires bounded queue; deadlock risk if pool size < DAG depth

### `ZephyrExecutor`
- **Target:** `Sub0Pipeline::Zephyr`
- **Backend:** `k_thread_create` / `k_sem` for completion tracking
- **Use case:** Zephyr RTOS (nRF, STM32, i.MX RT)
- **Notes:** Stack must be statically allocated; k_thread priority maps to priority field

### `AzureRtosExecutor`
- **Target:** `Sub0Pipeline::AzureRtos`
- **Backend:** `tx_thread_create` / `tx_semaphore`
- **Use case:** Azure RTOS ThreadX (STM32, RX, i.MX RT)
- **Notes:** Requires ThreadX memory pool pre-allocation

### `QtExecutor`
- **Target:** `Sub0Pipeline::Qt`
- **Backend:** `QThreadPool::globalInstance()->start(QRunnable*)` or `QtConcurrent::run`
- **Use case:** Qt-based desktop or embedded Linux applications
- **Notes:** Bridges into Qt's event loop; on_complete must be thread-safe

### `CoroutineExecutor`
- **Target:** `Sub0Pipeline::Coroutine`
- **Backend:** C++20 coroutines with cooperative scheduling
- **Use case:** Single-threaded async environments (WASM, embedded event loops)
- **Notes:** No true parallelism; excellent for I/O-bound pipelines

---

## Contributing a New Executor

1. Create `platform/<name>/<name>_executor.cpp`
2. Implement `IExecutor`: `dispatch()`, `wait_all()`, `concurrency()`
3. Expose a factory: `std::unique_ptr<IExecutor> make_<name>_executor()`
4. Add `platform/<name>/CMakeLists.txt` with a `Sub0Pipeline::<Name>` alias target
5. Wire the CMake option `SUB0PIPELINE_PLATFORM_<NAME>` in the root `CMakeLists.txt`
6. Add tests to `tests/test_concurrent.cpp` exercising the new executor

See `platform/desktop/desktop_executor.cpp` as a reference implementation.
