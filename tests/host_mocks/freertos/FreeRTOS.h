#pragma once

// Host declarations mirror SDK include names; they are never installed.
#include <cstddef>
#include <cstdint>

using BaseType_t = int;
using TickType_t = uint32_t;
inline constexpr BaseType_t pdPASS = 1;
inline constexpr BaseType_t pdFAIL = 0;
inline constexpr BaseType_t tskNO_AFFINITY = -1;
inline constexpr int portNUM_PROCESSORS = 2;
#define pdMS_TO_TICKS(value) (value)

std::size_t xPortGetFreeHeapSize();
