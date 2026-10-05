// include/sub0pipeline/executor/desktop_executor.hpp
//
// DesktopExecutor — one native thread per job.
// Link Sub0Pipeline::Desktop.
#pragma once

#include <sub0pipeline/executor/executor.hpp>

#include <memory>

namespace sub0pipeline {

/// Returns a `DesktopExecutor` (one `std::thread` per job, no priority ordering).
/// Defined in `platform/desktop/`.
std::unique_ptr<IExecutor> makeDesktopExecutor();

} // namespace sub0pipeline
