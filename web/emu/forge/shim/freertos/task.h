#pragma once
// Browser emulator: tasks are fibers run by the main loop (web/emu/emu_tasks.c)
#include "freertos/FreeRTOS.h"
void vTaskDelay(TickType_t ms);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ms);
BaseType_t xTaskNotifyGive(TaskHandle_t h);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core);
#define xTaskCreate(fn, name, stack, arg, prio, out) xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, 0)
bool emu_in_task(void);
void emu_tasks_run(void);
static inline TickType_t xTaskGetTickCount(void) { return (TickType_t)emscripten_get_now(); }
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 4096; }
static inline void vTaskDelete(TaskHandle_t t) { (void)t; }
