# Sub0Pipeline Project Rules

## Build & Test

```bash
cmake --preset default          # Configure (Release + tests + examples + benchmarks)
cmake --build --preset default  # Build
ctest --preset default          # Run tests
```

Benchmarks are built with the `default` preset but not run by ctest:
```bash
./build/tests/Release/Sub0Pipeline_Bench   # Windows
./build/tests/Sub0Pipeline_Bench           # Linux/macOS
```

## Commit Rules

### API Changes
Any commit that changes the public API surface in `include/sub0pipeline/`
(umbrella: `sub0pipeline.hpp`) must document the change in the commit message. The public API includes:
- `sub0pipeline::Pipeline` — all public methods
- `sub0pipeline::Job` — all builder methods
- `sub0pipeline::IExecutor` / `sub0pipeline::IObserver` — virtual interfaces
- `sub0pipeline::PipelineError` / `sub0pipeline::JobStatus` enumerators
- `sub0pipeline::TickJob`
- Factory functions: `make_desktop_executor()`, `make_sequential_executor()`, `make_freertos_executor()`

### Style
Follow `STYLE_GUIDE.md` for all C++ code. Key points:
- 4 spaces, no tabs
- `SUB0PIPELINE_` prefix for all configuration macros
- `sub0pipeline` namespace (lowercase)
- Classes `PascalCase`, members `camelCase_` (trailing underscore)

### Tests
- All new features must have corresponding tests in `tests/`
- Performance-sensitive changes should be validated with `Sub0Pipeline_Bench`
- Tests must pass locally before committing: `ctest --preset default`
- A git pre-push hook runs tests automatically — set up with:
  `git config core.hooksPath .githooks`

## Platform Executors

| Executor | Target | Build |
|---|---|---|
| `SequentialExecutor` | Tests, bare-metal | `Sub0Pipeline::Headless` |
| `DesktopExecutor` | Desktop, CI | `Sub0Pipeline::Desktop` |
| `FreeRtosExecutor` | ESP32-P4 | ESP-IDF component only |

See `PLATFORM_ROADMAP.md` for planned future executors.

## Branch Strategy
- `main` is the integration branch. Use focused topic branches and pull requests
  for changes; there is no `develop` branch.
- Keep unfinished experiments outside tracked source/docs unless they have an
  explicit issue, owner and acceptance criteria.

## Repository Hygiene
- Do not commit interim handoff notes, scratch documents, temporary probes,
  generated build output or unfinished example code. Use ignored build
  directories or local scratch space.
- Keep reproducible benchmark evidence only with its method, source revisions,
  toolchain and host limits documented. Preserve intentional regression tests
  and historical benchmark evidence.
