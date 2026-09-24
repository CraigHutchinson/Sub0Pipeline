# Repository guidance

Sub0Pipeline is a product-agnostic C++23 scheduling library. Use generic firmware
or device examples in source, tests, documentation, issues and pull requests.
Do not introduce consumer product names or consumer-specific dependencies.

## Implementation

- Write clean, lean C++23. Prefer standard library facilities, RAII, value
  semantics, explicit ownership, concepts and std::expected where appropriate.
- Reuse existing abstractions and shared implementation paths. Avoid duplicated
  cancellation, timeout, status conversion or executor policy logic.
- Keep platform APIs in adapters. Do not add framework or RTOS dependencies to
  the core. State which execution contexts each API supports.
- Distinguish cancellation requests from completed work and safe reclamation.
  Never detach work that can still access borrowed state. Document callback,
  observer, executor, allocator and job lifetimes.
- Consider bounded pools, fixed storage, custom allocators, no-exception builds,
  stack usage and execution-time allocation for embedded changes. Do not claim
  allocation-free behavior merely because graph storage was reserved.
- Scheduler calls are task-context APIs unless explicitly documented otherwise.
  ISR extensions must use platform-proven bounded handoff primitives, with clear
  overflow, memory-ordering, wakeup and shutdown contracts. Standard atomics alone
  do not establish interrupt safety on every target.
- Keep version-next proposals separate from implemented guarantees. Avoid adding
  unused public abstractions solely to anticipate a possible extension.

## Validation and review

- Add regression tests for observable behavior and ownership risks. Prefer
  controlled executors, injected clocks and latches over timing assumptions.
- Run the relevant tests and example builds. Use ASan/UBSan for lifetime work and
  ThreadSanitizer where supported for concurrent state changes.
- Record what was actually tested, platform limitations and remaining gaps in
  the PR. A passing sanitizer run is evidence, not proof of race freedom.
- Keep commits focused and descriptions current. Do not close an issue while
  its acceptance criteria remain unfulfilled.
