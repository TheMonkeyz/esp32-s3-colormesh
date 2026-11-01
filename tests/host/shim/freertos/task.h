#pragma once
// Host shim: defined by the test that links a file calling it (test_map.c counts the decoder's pauses)
#include "freertos/FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
