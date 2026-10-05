// include/sub0pipeline/config.hpp
//
// Compile-time error-handling policy (SUB0PIPELINE_EXCEPTIONS / SUB0PIPELINE_THROW).
#pragma once

#include <exception>
#include <stdexcept>

// ── Compile-time error-handling policy ───────────────────────────────────────
// Define SUB0PIPELINE_EXCEPTIONS=1 (default on most desktop platforms) to throw
// std::runtime_error on hard errors (e.g. pool address overflow).
// Define SUB0PIPELINE_EXCEPTIONS=0 to call std::terminate instead (RTOS, bare-metal,
// or builds with -fno-exceptions).
//
// CMake: option(SUB0PIPELINE_EXCEPTIONS "Throw std::runtime_error on hard errors" ON)
//        then: target_compile_definitions(Sub0Pipeline PUBLIC SUB0PIPELINE_EXCEPTIONS=$<BOOL:${SUB0PIPELINE_EXCEPTIONS}>)
#if !defined(SUB0PIPELINE_EXCEPTIONS)
#  if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#    define SUB0PIPELINE_EXCEPTIONS 1
#  else
#    define SUB0PIPELINE_EXCEPTIONS 0
#  endif
#endif

#if SUB0PIPELINE_EXCEPTIONS
#  define SUB0PIPELINE_THROW(msg) throw ::std::runtime_error(msg)
#else
#  define SUB0PIPELINE_THROW(msg) ::std::terminate()
#endif
