#pragma once
// Browser emulator: queues for the fiber tasks (web/emu/emu_tasks.c); a wait for an item yields to the main loop
#include "freertos/FreeRTOS.h"
typedef struct emu_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ms);
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t ms);
void vQueueDelete(QueueHandle_t q);
