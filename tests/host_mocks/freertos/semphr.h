#pragma once

#include <freertos/FreeRTOS.h>

struct MockSemaphore;
using SemaphoreHandle_t = MockSemaphore*;
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned int maximum, unsigned int initial);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
