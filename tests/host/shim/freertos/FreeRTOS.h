#pragma once
// Host shim: FreeRTOS's tick type; the functions a tested file calls are the test's (vTaskDelay: freertos/task.h)
#include <stdint.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
