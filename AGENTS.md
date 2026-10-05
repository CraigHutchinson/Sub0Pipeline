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

## Repository hygiene

- Do not commit interim handoff notes, scratch documents, temporary probes,
  generated build output or unfinished example code. Keep transient work in
  ignored build directories or local scratch space.
- Retain benchmark captures only when they are reproducible evidence with the
  method, source revisions, toolchain and host limits documented. Keep design
  documents only when they describe an owned, current proposal or contract.
- Before delivery, inspect tracked and untracked changes for abandoned WIP files;
  do not remove deliberate tests or historical benchmark evidence.

## Performance and delivery workflow

- Follow [CONTRIBUTING.md](CONTRIBUTING.md) for the review and validation gates.
- For scheduler, allocation, synchronization or executor changes, capture a
  same-machine baseline and current Release build using the shared benchmark
  harness and `scripts/capture_benchmarks.py`. Keep at least five alternating
  process samples, raw JSON/logs, source refs, compiler/build and machine details.
- Profile before optimizing: attribute the time with `scripts/profile_vtune.py`
  (or an equivalent sampling profiler) and state the hypothesis under test.
  Profiles are for attribution only; never quote a timing from a profiled run.
- Report medians and observed ranges, feature settings and measurement limits.
  Investigate material regressions; explain intentional safety costs. Never use
  a faster result to justify weaker ownership or cancellation guarantees.
- Keep costly optional work opt-in: observer callbacks, external-stop forwarding,
  timeout helper threads, extra executor targets and benchmark runs. Required
  correctness checks may be cached only with complete invalidation tests.
- Audit memory behavior separately from timing. No universal “zero overhead”,
  “allocation-free”, “hard real-time” or optimality claims without evidence.
- Update the root README feature/cost matrix, compatibility notes and examples
  whenever the public behavior changes. Clearly label stubs and future features.
- Keep performance jobs manual/advisory on shared CI runners; compare controlled
  environments before applying a regression threshold. Do not weaken functional
  or sanitizer gates to make a performance result look better.
