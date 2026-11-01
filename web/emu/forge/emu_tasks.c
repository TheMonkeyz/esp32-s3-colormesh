// FreeRTOS tasks in the browser: each task is an Emscripten fiber with its own stacks, run cooperatively by the main
// loop (emu_tasks_run, between LVGL frames). A wait in a task (vTaskDelay, ulTaskNotifyTake, an HTTP request) records
// when it wants to run again and switches back to the main loop; a wait in the main loop is an emscripten_sleep. So
// main.c's app_main (emu_loop.c) and the app's own tasks run unchanged: LVGL never runs while a task does, as the
// display lock guarantees on the board. At most MAX_TASKS, each with C_STACK of C stack whatever the firmware asks
// (a fiber's stack also holds what ASYNCIFY saves).
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <emscripten.h>
#include <emscripten/fiber.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define MAX_TASKS 6
#define C_STACK (256 * 1024)
#define ASYNC_STACK (64 * 1024)

typedef struct emu_task {
    emscripten_fiber_t fiber;
    void (*fn)(void *);
    void *arg;
    int64_t wake;              // run again from then on
    bool on_notify;            // ...or as soon as notified
    uint32_t notified;
    bool done;
    char name[16];
} emu_task_t;

static emu_task_t tasks[MAX_TASKS];
static int n_tasks;
static emscripten_fiber_t main_fiber;
static bool main_ready;
static emu_task_t *current;     // the task running now (NULL: the main loop)

static void entry(void *arg)
{
    emu_task_t *t = arg;
    t->fn(t->arg);
    t->done = true;
    for (;;) emscripten_fiber_swap(&t->fiber, &main_fiber);
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core)
{
    (void)stack; (void)prio; (void)core;
    if (!main_ready) {
        static uint8_t main_async[ASYNC_STACK];
        emscripten_fiber_init_from_current_context(&main_fiber, main_async, sizeof(main_async));
        main_ready = true;
    }
    if (n_tasks == MAX_TASKS) return pdFALSE;
    emu_task_t *t = &tasks[n_tasks++];
    memset(t, 0, sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    strncpy(t->name, name, sizeof(t->name) - 1);
    emscripten_fiber_init(&t->fiber, entry, t, malloc(C_STACK), C_STACK, malloc(ASYNC_STACK), ASYNC_STACK);
    if (out) *out = t;
    return pdPASS;
}

// The main loop: run every task that is due (each until its next wait)
void emu_tasks_run(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < n_tasks; i++) {
        emu_task_t *t = &tasks[i];
        if (t->done || (now < t->wake && !(t->on_notify && t->notified))) continue;
        current = t;
        emscripten_fiber_swap(&main_fiber, &t->fiber);
        current = NULL;
    }
}

static void task_wait(TickType_t ms, bool on_notify)
{
    emu_task_t *t = current;
    t->wake = esp_timer_get_time() + (ms == portMAX_DELAY ? 3600000000LL : (int64_t)ms * 1000);
    t->on_notify = on_notify;
    emscripten_fiber_swap(&t->fiber, &main_fiber);
    t->on_notify = false;
}

void vTaskDelay(TickType_t ms)
{
    if (current) task_wait(ms ? ms : 1, false);
    else emscripten_sleep(ms ? ms : 1);
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ms)
{
    if (!current) { emscripten_sleep(ms > 20 ? 20 : ms ? ms : 1); return 0; }
    emu_task_t *t = current;
    if (!t->notified && ms) task_wait(ms, true);
    uint32_t v = t->notified;
    if (v) t->notified = clear ? 0 : v - 1;
    return v;
}

BaseType_t xTaskNotifyGive(TaskHandle_t h)
{
    if (h) ((emu_task_t *)h)->notified++;
    return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }

/* ---------- queues (the starter's ui.c: the setup radio's requests) ---------- */
#include "freertos/queue.h"
struct emu_queue { int len, size, n, head; uint8_t *items; };

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t size)
{
    struct emu_queue *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->len = (int)len;
    q->size = (int)size;
    q->items = calloc(len, size);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ms)
{
    (void)ms;
    if (q->n == q->len) return pdFALSE;
    memcpy(q->items + ((q->head + q->n) % q->len) * q->size, item, q->size);
    q->n++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t ms)
{
    int64_t end = ms == portMAX_DELAY ? INT64_MAX : esp_timer_get_time() + (int64_t)ms * 1000;
    while (!q->n) {
        if (esp_timer_get_time() >= end) return pdFALSE;
        vTaskDelay(20);                                  // a task: back to the main loop meanwhile
    }
    memcpy(out, q->items + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->len;
    q->n--;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t q)
{
    if (q) { free(q->items); free(q); }
}

bool emu_in_task(void) { return current != NULL; }

/* ---------- binary semaphores (a task tells another it is done; the starter's ui.c: su_dpp_off) ---------- */
#include "freertos/semphr.h"
struct emu_sem { int count; };

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1, sizeof(struct emu_sem)); }

BaseType_t emu_sem_give(SemaphoreHandle_t h)
{
    struct emu_sem *s = h;
    if ((uintptr_t)h <= 1) return pdTRUE;                // a mutex (semphr.h): never contended
    if (s->count) return pdFALSE;                        // binary: already given
    s->count = 1;
    return pdTRUE;
}

// A wait in a task goes back to the main loop; in the main loop (a settings page request waiting for a task's answer)
// it runs the tasks itself until the semaphore is given or the time is up
BaseType_t emu_sem_take(SemaphoreHandle_t h, TickType_t ms)
{
    struct emu_sem *s = h;
    if ((uintptr_t)h <= 1) return pdTRUE;
    int64_t end = ms == portMAX_DELAY ? INT64_MAX : esp_timer_get_time() + (int64_t)ms * 1000;
    while (!s->count) {
        if (esp_timer_get_time() >= end) return pdFALSE;
        if (current) vTaskDelay(5);
        else { emu_tasks_run(); emscripten_sleep(5); }
    }
    s->count = 0;
    return pdTRUE;
}
