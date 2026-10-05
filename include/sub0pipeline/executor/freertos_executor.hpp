// include/sub0pipeline/executor/freertos_executor.hpp
//
// FreeRtosExecutor — one FreeRTOS task per job.
// Built only as the esp32p4 ESP-IDF component.
#pragma once

#include <sub0pipeline/executor/executor.hpp>

#include <memory>

namespace sub0pipeline {

/// Returns a `FreeRtosExecutor` (one FreeRTOS task per job). Defined in
/// `platform/esp32p4/`, which builds only as an ESP-IDF component.
std::unique_ptr<IExecutor> makeFreeRtosExecutor();

} // namespace sub0pipeline
