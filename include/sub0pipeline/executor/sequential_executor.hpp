// include/sub0pipeline/executor/sequential_executor.hpp
//
// SequentialExecutor — runs jobs on the calling thread, no threads.
// Link Sub0Pipeline::Headless.
#pragma once

#include <sub0pipeline/executor/executor.hpp>

#include <memory>

namespace sub0pipeline {

/// Returns a `SequentialExecutor` (inline, no threads, deterministic).
/// Defined in `platform/headless/`.
std::unique_ptr<IExecutor> makeSequentialExecutor();

} // namespace sub0pipeline
