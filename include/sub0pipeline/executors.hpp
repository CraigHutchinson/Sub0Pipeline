// include/sub0pipeline/executors.hpp
//
// Every executor header: the IExecutor interface, ScopedExecutor, each bundled
// executor class, and DefaultExecutor, which names the one that suits the
// platform. Each header states the library its executor needs.
#pragma once

#include "sub0pipeline/executor/default_executor.hpp"
#include "sub0pipeline/executor/desktop_executor.hpp"
#include "sub0pipeline/executor/executor.hpp"
#include "sub0pipeline/executor/freertos_executor.hpp"
#include "sub0pipeline/executor/priority_executor.hpp"
#include "sub0pipeline/executor/scoped_executor.hpp"
#include "sub0pipeline/executor/sequential_executor.hpp"
