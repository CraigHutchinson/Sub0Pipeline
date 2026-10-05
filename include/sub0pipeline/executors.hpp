// include/sub0pipeline/executors.hpp
//
// Every executor header: the IExecutor interface, ScopedExecutor, and the
// factory for each bundled executor. Including a factory header costs nothing
// until you call it; each one names the library to link.
#pragma once

#include <sub0pipeline/executor/desktop_executor.hpp>
#include <sub0pipeline/executor/executor.hpp>
#include <sub0pipeline/executor/freertos_executor.hpp>
#include <sub0pipeline/executor/priority_executor.hpp>
#include <sub0pipeline/executor/scoped_executor.hpp>
#include <sub0pipeline/executor/sequential_executor.hpp>
