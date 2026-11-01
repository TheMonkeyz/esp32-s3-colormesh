#pragma once
// Browser emulator: one thread. A wait hands control back to the browser (Emscripten ASYNCIFY), so the loops that follow
// the finger (slide.c) keep drawing and reading the mouse.
#include <stdint.h>
#include <stdbool.h>
#include <emscripten.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
