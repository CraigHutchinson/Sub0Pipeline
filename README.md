# Sub0Pipeline

> Zero-overhead DAG job scheduler for boot sequencing and application lifecycle.
> Part of the **Sub0** C++ library family.

[![CI](https://github.com/your-org/Sub0Pipeline/actions/workflows/ci.yml/badge.svg)](https://github.com/your-org/Sub0Pipeline/actions)

---

## Overview

Sub0Pipeline is a C++23 library for declaring and executing directed acyclic
graphs (DAGs) of jobs. Independent jobs run in parallel via a platform-injectable
executor; dependent jobs wait for their predecessors.

Designed for:
- **Embedded boot sequencing** — ESP32-P4, Zephyr, bare-metal
- **Desktop simulation** — std::thread-backed parallel execution
- **Deterministic testing** — inline sequential executor, no threads

Extracted from NestNinja (ADR-027) as a standalone Sub0-family library, with
consistent structure and coding style mirroring [Sub0Pub](https://github.com/your-org/Sub0Pub).

---

## Quick Start

```cpp
#include <sub0pipeline/sub0pipeline.hpp>

sub0pipeline::Pipeline boot;

auto nvs     = boot.emplace([] { return nvs_init();     }).name("nvs");
auto display = boot.emplace([] { return display_init(); }).name("display").timeout(8s);
auto network = boot.emplace([] { return network_init(); }).name("network").timeout(10s);
auto app     = boot.emplace([] { return app_start();    }).name("app");

// display and network run in parallel (no mutual dependency)
display.succeed(nvs);
network.succeed(nvs);
app.succeed(display, network);

auto exec   = sub0pipeline::make_desktop_executor();
auto result = boot.run(*exec);
```

---

## Build

```bash
cmake --preset default          # Configure Release + tests + examples
cmake --build --preset default  # Build
ctest --preset default          # Run tests
```

Presets: `default` (Release), `debug`, `ci-msvc`, `ci-unix`, `ci-asan`

---

## Project Structure

```
include/sub0pipeline/sub0pipeline.hpp   # Public API (Pipeline, Job, IExecutor, …)
src/sub0pipeline.cpp                     # DAG engine implementation
platform/
  desktop/   desktop_executor.cpp        # std::thread executor
  headless/  sequential_executor.cpp     # Inline executor (tests, bare-metal)
  esp32p4/   freertos_executor.cpp       # FreeRTOS/ESP32-P4 executor
tests/
  test_pipeline.cpp   test_failure.cpp   test_validation.cpp
  test_observer.cpp   test_concurrent.cpp
  bench_pipeline.cpp                     # nanobench benchmarks
examples/
  minimal_pipeline/  boot_sequence/  parallel_tasks/  on_demand_jobs/
```

---

## Executors

| Executor | Factory | Target |
|---|---|---|
| Sequential (inline) | `make_sequential_executor()` | `Sub0Pipeline::Headless` |
| Desktop (std::thread) | `make_desktop_executor()` | `Sub0Pipeline::Desktop` |
| FreeRTOS (ESP32-P4) | `make_freertos_executor()` | ESP-IDF component |

See [PLATFORM_ROADMAP.md](PLATFORM_ROADMAP.md) for planned executors.

---

## CMake Integration

```cmake
add_subdirectory(Sub0Pipeline)
target_link_libraries(MyApp PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Desktop)
```

Or via `find_package` after install:
```cmake
find_package(Sub0Pipeline REQUIRED)
target_link_libraries(MyApp PRIVATE Sub0Pipeline::Sub0Pipeline)
```

---

## API

### Pipeline

| Method | Description |
|---|---|
| `emplace(fn)` | Add a job; returns a `Job` handle |
| `run(executor, observer?)` | Execute DAG; returns `expected<void, PipelineError>` |
| `validate()` | Check for cycles (called automatically by `run()`) |
| `status(job)` | Query current `JobStatus` |
| `name(job)` | Query job name |
| `add_tick(tick)` | Register a recurring tick job |
| `run_loop()` | Enter the post-boot event loop (noreturn) |
| `add_on_demand(fn)` | Register an event-triggered job |
| `dump_text()` | Print DAG structure to stdout |

### Job (fluent builder)

```cpp
job.name("nvs").timeout(5s).core(1).priority(10).optional().succeed(other);
```

---

## Sister Libraries

- **[Sub0Pub](https://github.com/your-org/Sub0Pub)** — Zero-overhead typed publish-subscribe

---

## License

MIT — see [LICENSE.md](LICENSE.md)
