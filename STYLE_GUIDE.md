# Sub0Pipeline Code Style Guide

Style conventions derived from the Sub0 family codebase (Sub0Pub). Follow these
for consistency across Sub0Pipeline, Sub0Pub, and related libraries.

---

## Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Namespaces | lowercase | `sub0pipeline`, `detail` |
| Classes/Structs | PascalCase | `Pipeline`, `Job`, `IExecutor`, `TickJob` |
| Template parameters | PascalCase with `_t` suffix for type params | `Data_t`, `Fn` |
| Member variables | camelCase with `_` suffix | `inFlight_`, `completionSem_`, `nameStr_` |
| Local variables | camelCase | `dispatchJob`, `hasFatalFailure` |
| Constants | `c` prefix + PascalCase | `cInvalid`, `cMaxWorkers` |
| Macros/Defines | UPPER_SNAKE_CASE with `SUB0PIPELINE_` prefix | `SUB0PIPELINE_TRACE` |
| Free functions | camelCase | `make_desktop_executor()`, `make_sequential_executor()` |
| Type aliases | PascalCase | `JobFn`, `CompleteFn` |

## Formatting

- **Indentation:** 4 spaces (no tabs)
- **Braces:** Opening brace on same line for control flow, next line for class/function definitions
- **Line width:** ~120 characters soft limit
- **Pointer/reference alignment:** `Type* name` (pointer with type), `const Type& name`

```cpp
// Class definition
class DesktopExecutor final : public IExecutor
{
public:
    void dispatch(
        std::string_view name,
        std::function<void()> fn,
        std::function<void()> on_complete,
        int core, uint8_t priority, uint32_t stack_bytes) override
    {
        inFlight_.fetch_add(1U, std::memory_order_relaxed);
        // ...
    }
};
```

## Templates

- Use angle brackets with space inside for readability: `template< typename F >`
- Use `using` aliases over `typedef`
- Concept constraints preferred over SFINAE: `requires std::invocable<F>`

## Documentation

- Doxygen-style `/** */` for public API
- `@param[in]`, `@return`, `@remark`, `@note`, `@warning` tags
- Inline `///<` for member variable documentation
- Brief `//` comments for implementation rationale

## Preprocessor

- Feature flags use `#ifndef` / `#define` / `#endif` with default values
- Guard conditions: `#if SUB0PIPELINE_FLAG` (not `#ifdef`)
- Platform detection: `#if __has_include(<freertos/FreeRTOS.h>)`

## Error Handling

- Return types: `std::expected<void, PipelineError>` for fallible operations
- Never throw in dispatch/execution hot path
- `assert()` for internal invariants during debug builds

## Integer Types

- Use `<cstdint>` fixed-width types: `uint32_t`, `uint8_t`
- Unsigned literals with `U` suffix: `0U`, `8U`, `4096U`
- Cast explicitly when narrowing: `static_cast<uint32_t>(nodes.size())`

## Includes

- Standard library includes sorted alphabetically
- Project includes use angle brackets: `#include <sub0pipeline/sub0pipeline.hpp>`
- Platform-specific includes guarded: `#if __has_include(<esp_log.h>)`
