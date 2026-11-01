#pragma once
// Browser emulator: one thread, tasks are fibers (web/emu/emu_tasks.c). A mutex is never contended (a task only gives
// way at a wait, and no lock is held across one that another needs), so it always succeeds. A binary semaphore is
// real: a task gives it while another task, or a settings page handler, waits for it; a wait in the main loop runs
// the tasks meanwhile.
#include "freertos/FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t emu_sem_take(SemaphoreHandle_t s, TickType_t ms);
BaseType_t emu_sem_give(SemaphoreHandle_t s);
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (SemaphoreHandle_t)1; }
#define xSemaphoreTake(s, t) emu_sem_take((s), (t))
#define xSemaphoreGive(s) emu_sem_give(s)
#define xSemaphoreTakeRecursive(s, t) pdTRUE
#define xSemaphoreGiveRecursive(s) pdTRUE
