#pragma once

#include <freertos/FreeRTOS.h>

using TaskFunction_t = void (*)(void*);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char* name,
    uint32_t stack, void* context, uint8_t priority, void** task, BaseType_t affinity);
void vTaskDelete(void* task);
