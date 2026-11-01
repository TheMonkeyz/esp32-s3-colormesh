// A background worker for small network jobs (net.h: net_bg_post). One task, started for the first job and gone
// after 30 s without one, runs them one at a time inside the download slot. MeteoBus's lesson (its departures poller):
// a task per poller costs ~6 KB of internal RAM each, a PSRAM stack stopped a TLS task for good once, and two
// downloads at once took the internal low point from ~40 to 29 KB. svc's probes run here.
#include "net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "netbg";
#define BG_QUEUE   8
#define BG_STACK   8192                   // internal RAM: a TLS handshake runs here (svc's probe task had 8 KB)
#define BG_IDLE_MS 30000

typedef struct { void (*fn)(void *arg); void *arg; const char *who; } job_t;
static QueueHandle_t q;
static SemaphoreHandle_t mu;               // q, task and running: posting against the task's exit
static TaskHandle_t task;
static volatile int running;

static void bg_task(void *arg)
{
    job_t j;
    for (;;) {
        if (xQueueReceive(q, &j, pdMS_TO_TICKS(BG_IDLE_MS)) != pdTRUE) {
            xSemaphoreTake(mu, portMAX_DELAY);
            if (uxQueueMessagesWaiting(q) == 0) {          // (a job posted meanwhile keeps the task)
                task = NULL;
                xSemaphoreGive(mu);
                ESP_LOGI(TAG, "idle %d s: worker gone (stack spare %u B)", BG_IDLE_MS / 1000,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
                vTaskDelete(NULL);
            }
            xSemaphoreGive(mu);
            continue;
        }
        running = 1;
        if (net_dl_begin(j.who, 30000)) {
            j.fn(j.arg);
            net_dl_end(j.who);
        } else {
            ESP_LOGW(TAG, "%s skipped: the download slot stayed busy", j.who);
        }
        running = 0;
    }
}

bool net_bg_post(void (*fn)(void *arg), void *arg, const char *who)
{
    if (!fn) return false;
    if (!mu) mu = xSemaphoreCreateMutex();                 // (the first post is from app start-up, one task)
    xSemaphoreTake(mu, portMAX_DELAY);
    if (!q) q = xQueueCreate(BG_QUEUE, sizeof(job_t));
    job_t j = { fn, arg, who ? who : "job" };
    bool ok = q && xQueueSend(q, &j, 0) == pdTRUE;
    if (ok && !task && xTaskCreate(bg_task, "netbg", BG_STACK, NULL, 2, &task) != pdPASS) {
        task = NULL;
        ok = false;
        xQueueReceive(q, &j, 0);                           // not going to run: take it back
    }
    xSemaphoreGive(mu);
    if (!ok) ESP_LOGW(TAG, "%s not queued (%s)", j.who, q ? "queue full or no task" : "no queue");
    return ok;
}

int net_bg_queued(void) { return (q ? (int)uxQueueMessagesWaiting(q) : 0) + running; }
