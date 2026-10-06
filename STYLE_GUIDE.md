# Sub0Pipeline Code Style Guide

style-profile: sub0

Sub0Pipeline follows the Sub0 family style profile. This guide summarises how
the rules apply here; where it and the profile disagree, the profile wins. The
profile lives with the family tooling (`sub0-profile.md`, selected by the
`style-profile:` line above) and numbers each rule (S01 and so on).

---

## Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Namespaces | lowercase | `sub0pipeline`, `detail` |
| Classes/Structs/Enums/Aliases | PascalCase | `Pipeline`, `Job`, `TickLoop`, `JobFn` |
| Interfaces (pure virtual) | `I` prefix | `IExecutor`, `IObserver` |
| Functions and methods | camelCase | `waitAll()`, `runInline()`, `parallel()` |
| Member variables | camelCase with `_` suffix | `inFlight_`, `completionSem_` |
| Local variables | camelCase | `dispatchJob`, `hasFatalFailure` |
| Constants and enumerators | prefix + PascalCase (see Open) | `cInvalid`, `kTimeout` |
| Macros | UPPER_SNAKE_CASE with `SUB0PIPELINE_` prefix | `SUB0PIPELINE_TRACE` |
| Files | snake_case `.hpp` / `.cpp` | `job_group.hpp`, `test_cancel.cpp` |

Names that mirror the standard library's protocols keep the standard's
spelling so generic code keeps working: `request_stop()` on `RunScope`
mirrors `std::stop_source`. Calls into the standard library are unaffected.

## Formatting

- 4 spaces, no tabs.
- Allman braces: the opening brace is on its own line for namespaces, types,
  functions and control flow (`if`, `else`, `for`, `while`, `switch`, `try`,
  `catch`). `else` and `catch` start their own line after the closing brace.
  Single-line bodies (`if (x) return;`, `{ return x_; }`) and lambda bodies
  are not covered.
- Line width: about 120 characters, soft limit.
- Pointer and reference: `Type* name`, `const Type& name`.

```cpp
class DesktopExecutor final : public IExecutor
{
public:
    void waitAll() override
    {
        std::unique_lock lock{mtx_};
        if (inFlight_ != 0U)
        {
            idle_.wait(lock, [this] { return inFlight_ == 0U; });
        }
    }
};
```

## Includes

- The library's own headers use quotes and a library-rooted path:
  `#include "sub0pipeline/job.hpp"`. Never bare relative (`"job.hpp"`) and
  never angle brackets.
- System and third-party headers use angle brackets, including the vendored
  test dependencies: `#include <doctest.h>`.
- Order: system and third-party headers first, then the library's own. Each
  group is alphabetical, with one blank line between the groups. A file's own
  header is not listed first; the `Sub0Pipeline_HeaderCheck` target is what
  proves each public header is self-contained.
- Platform-specific includes are guarded: `#if __has_include(<esp_log.h>)`.
- Header guards are `#pragma once`.

## Documentation

- Every public declaration has a Doxygen `/** ... */` comment. There is no
  `@brief`: the first sentence is the brief and ends in a full stop.
- `@param` for every parameter, `@return` for every non-void result,
  `@tparam` for caller-supplied template parameters. Add `@note` for thread
  safety, ownership and lifetime where they matter. Directional tags such as
  `@param[in]` are allowed but not required.
- Exempt from the tags: `override` declarations (they inherit the base
  documentation), defaulted or deleted special members, and operators with
  their conventional meaning.
- Plain `//` comments are for private members and implementation rationale.
- Inline `///<` documents data members.
- Use `[[nodiscard]]` where discarding the result is a wasted computation or
  a likely usage error (queries, factories, status returns). Not on fluent
  builder methods or functions called for their effect.

## Templates

- Use `using` aliases over `typedef`.
- Prefer concept constraints over SFINAE: `requires std::invocable<F>`.

## Preprocessor

- Feature flags use `#ifndef` / `#define` / `#endif` with default values.
- Guard conditions: `#if SUB0PIPELINE_FLAG` (not `#ifdef`).

## Error Handling

- Return `std::expected<void, PipelineError>` for fallible operations.
- Never throw in the dispatch/execution hot path.
- `assert()` for internal invariants in debug builds.

## Integer Types

- Use `<cstdint>` fixed-width types: `uint32_t`, `uint8_t`.
- Unsigned literals with a `U` suffix: `0U`, `8U`, `4096U`.
- Cast explicitly when narrowing: `static_cast<uint32_t>(nodes.size())`.

## Open in the family profile

These are undecided there, so this guide does not fix them: the prefix on
constants and enumerators (`c`, `k` today), the namespace scheme, the spacing
inside `template<...>` (the code uses `template<typename F>`), the error
handling style, how test dependencies are supplied, and
whether to add a formatter configuration.
