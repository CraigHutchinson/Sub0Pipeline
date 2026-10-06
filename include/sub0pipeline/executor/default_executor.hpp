// include/sub0pipeline/executor/default_executor.hpp
//
// DefaultExecutor — the bundled executor that suits the platform being built.
// Link Sub0Pipeline::Default, which carries the library that executor needs.
#pragma once

// The choice is made here, at compile time, so code that names DefaultExecutor
// moves between platforms without change:
//
//   FreeRTOS headers present  -> FreeRtosExecutor  (one task per job)
//   standard threads present  -> PriorityExecutor  (bounded worker pool)
//   otherwise                 -> SequentialExecutor (calling thread only)
//
// A build that has threads but was configured without the priority pool sets
// SUB0PIPELINE_DEFAULT_EXECUTOR_SEQUENTIAL through Sub0Pipeline::Default, so
// the header and the linked libraries always agree.

#if !defined(SUB0PIPELINE_DEFAULT_EXECUTOR_SEQUENTIAL)
#  define SUB0PIPELINE_DEFAULT_EXECUTOR_SEQUENTIAL 0
#endif

#if SUB0PIPELINE_DEFAULT_EXECUTOR_SEQUENTIAL
#  include "sub0pipeline/executor/sequential_executor.hpp"
#elif __has_include(<freertos/FreeRTOS.h>)
#  include "sub0pipeline/executor/freertos_executor.hpp"
#elif defined(__STDCPP_THREADS__)
#  include "sub0pipeline/executor/priority_executor.hpp"
#else
#  include "sub0pipeline/executor/sequential_executor.hpp"
#endif

namespace sub0pipeline {

/**
 * @brief The bundled executor selected for the platform being built.
 *
 * An alias for FreeRtosExecutor, PriorityExecutor or SequentialExecutor; see
 * the top of this header for the rule. Every candidate is default-constructible
 * and usable through IExecutor, so portable code constructs one with no
 * arguments and passes it to Pipeline::run():
 * @code
 *   sub0pipeline::DefaultExecutor executor;
 *   auto result = pipeline.run(executor);
 * @endcode
 *
 * Name a specific executor class instead when the code depends on its
 * behavior or its options, such as PriorityExecutor::Options.
 */
#if SUB0PIPELINE_DEFAULT_EXECUTOR_SEQUENTIAL
using DefaultExecutor = SequentialExecutor;
#elif __has_include(<freertos/FreeRTOS.h>)
using DefaultExecutor = FreeRtosExecutor;
#elif defined(__STDCPP_THREADS__)
using DefaultExecutor = PriorityExecutor;
#else
using DefaultExecutor = SequentialExecutor;
#endif

} // namespace sub0pipeline
