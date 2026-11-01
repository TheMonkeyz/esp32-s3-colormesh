// Diagnostics logger (see diag.h)
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_core_dump.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "nvs.h"
#include "esp_app_desc.h"

static const char *TAG = "diag";

// Every failed heap allocation is counted (shown in the heap lines and the test console's "heap"): several
// callers drop a failed allocation silently (a sound not played, a map not drawn), and the harness wants it at 0.
static volatile uint32_t failed_allocs;
static void alloc_failed(size_t size, uint32_t caps, const char *fn) { failed_allocs++; }
uint32_t diag_failed_allocs(void) { return failed_allocs; }
uint32_t lvgl_mem_fallbacks(void);       // testcon.c (weak 0) or forge_lvgl's lvgl_mem.c

#define MAX_HOOKS 4
static diag_hook_t hooks[MAX_HOOKS];
static int nhooks;
void diag_add_hook(diag_hook_t hook) { if (nhooks < MAX_HOOKS) hooks[nhooks++] = hook; }

#define MAX_TASKS 32
#define KB(x) ((unsigned)((x) / 1024))
static TaskStatus_t ts[MAX_TASKS];                                // report_tasks and the start-up timing

typedef struct { UBaseType_t num; uint32_t rt; } prev_t;   // by task number: a freed handle is reused
static prev_t prev[MAX_TASKS];
static int nprev;
static uint32_t prev_total;

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT WDT";
    case ESP_RST_TASK_WDT: return "TASK WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "usb";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
    }
}

static void startup_info(void)
{
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    ESP_LOGI(TAG, "firmware %s (built %s %s)", esp_app_get_description()->version,
             esp_app_get_description()->date, esp_app_get_description()->time);
    ESP_LOGI(TAG, "boot reset=%s flash_mb=%u psram_kb=%u", reset_reason(), (unsigned)(flash >> 20),
             KB(esp_psram_get_size()));
    const esp_partition_t *app = esp_ota_get_running_partition();      // (the first app partition was printed)
    if (app) ESP_LOGI(TAG, "running from %s, %u KB at 0x%lx", app->label, KB(app->size), (unsigned long)app->address);
    // The last crash, if the core dump partition holds one: the task, the PC and the backtrace (decode it with
    // xtensa-esp32s3-elf-addr2line -pfC -e build/v55/<app>.elf <addresses>; on RISC-V riscv32-esp-elf-addr2line with
    // the PC and RA), then erased
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    esp_core_dump_summary_t *cd = malloc(sizeof(*cd));
    esp_err_t chk = esp_core_dump_image_check();
    if (chk != ESP_OK && chk != ESP_ERR_NOT_FOUND) {
        // Not a crash: whatever was in the partition before (another firmware's data at that address after a new
        // partition table). ESP-IDF logs an E line about it at every boot until it is erased. But a blank partition
        // (size word 0xFFFFFFFF, which is also what an erase leaves) gives the same ESP_ERR_INVALID_SIZE: read the
        // word, or every boot after the first erase erased again (found aligning with weather_amoled, October 4)
        const esp_partition_t *cp = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        uint32_t size = 0xFFFFFFFF;
        if (cp && esp_partition_read(cp, 0, &size, sizeof(size)) == ESP_OK && size != 0xFFFFFFFF) {
            ESP_LOGW(TAG, "coredump: partition held no valid dump (%s, size word 0x%08lx), erased", esp_err_to_name(chk),
                     (unsigned long)size);
            esp_core_dump_image_erase();
        }
    }
    if (cd && chk == ESP_OK && esp_core_dump_get_summary(cd) == ESP_OK) {
#if __XTENSA__
        char bt[16 * 11 + 1] = "";
        for (uint32_t i = 0, n = 0; i < cd->exc_bt_info.depth && i < 16; i++)
            n += snprintf(bt + n, sizeof(bt) - n, " 0x%08lx", (unsigned long)cd->exc_bt_info.bt[i]);
        ESP_LOGW(TAG, "coredump: last crash in task %s, PC 0x%08lx, backtrace%s%s", cd->exc_task,
                 (unsigned long)cd->exc_pc, bt, cd->exc_bt_info.corrupted ? " (corrupted)" : "");
#else   // RISC-V (ESP32-P4): no backtrace in the summary, only the registers (addr2line the PC and RA)
        ESP_LOGW(TAG, "coredump: last crash in task %s, PC 0x%08lx, RA 0x%08lx, mcause 0x%lx, mtval 0x%08lx",
                 cd->exc_task, (unsigned long)cd->exc_pc, (unsigned long)cd->ex_info.ra,
                 (unsigned long)cd->ex_info.mcause, (unsigned long)cd->ex_info.mtval);
#endif
        esp_core_dump_image_erase();
    }
    free(cd);
#endif
    // What each statistic costs here, once. Both hold interrupts off (L201): on a board whose panel is restarted from
    // an interrupt every frame (an ESP32-P4's MIPI-DSI, ~0.9 ms between frames) they made the screen flash white, so
    // each has a Kconfig switch (FORGE_DIAG_TASK_STATS, FORGE_DIAG_PSRAM_LARGEST). ESP32-S3 with 8 MB: both well
    // under a millisecond; a P4 with 25 tasks and 32 MB: 6.1 and 2.3 ms (MeteoBus, 2026-10-30).
    int64_t t_tasks = -1, t_psram = -1;
#if CONFIG_FORGE_DIAG_TASK_STATS
    {
        uint32_t total;
        int64_t t0 = esp_timer_get_time();
        uxTaskGetSystemState(ts, MAX_TASKS, &total);
        t_tasks = esp_timer_get_time() - t0;
    }
#endif
#if CONFIG_FORGE_DIAG_PSRAM_LARGEST
    {
        int64_t t0 = esp_timer_get_time();
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        t_psram = esp_timer_get_time() - t0;
    }
#endif
    ESP_LOGI(TAG, "stats cost: task list %lld us, PSRAM largest block %lld us (-1: off by Kconfig)",
             (long long)t_tasks, (long long)t_psram);
    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK)
        ESP_LOGI(TAG, "NVS entries: %u used, %u free of %u", (unsigned)ns.used_entries,
                 (unsigned)ns.free_entries, (unsigned)ns.total_entries);
}

static int cmp_rt(const void *a, const void *b)
{
    const TaskStatus_t *x = a, *y = b;                            // counters hold deltas here
    return (y->ulRunTimeCounter > x->ulRunTimeCounter) - (y->ulRunTimeCounter < x->ulRunTimeCounter);
}

static void report_tasks(void)
{
    uint32_t total;
    int n = uxTaskGetSystemState(ts, MAX_TASKS, &total);
    uint32_t window = total - prev_total;                         // µs of wall time (per core)
    // Replace each counter by its delta since the last report
    prev_t now[MAX_TASKS];
    for (int i = 0; i < n; i++) {
        uint32_t rt = ts[i].ulRunTimeCounter, old = 0;
        for (int j = 0; j < nprev; j++) if (prev[j].num == ts[i].xTaskNumber) { old = prev[j].rt; break; }
        now[i].num = ts[i].xTaskNumber; now[i].rt = rt;
        ts[i].ulRunTimeCounter = prev_total ? rt - old : 0;
    }
    memcpy(prev, now, sizeof(prev_t) * n);
    nprev = n;
    prev_total = total;
    if (!window || window == total) return;                      // first call: baseline only
    qsort(ts, n, sizeof(TaskStatus_t), cmp_rt);

    float idle[2] = {0, 0};
    char line[200];
    int len = 0;
    for (int i = 0; i < n; i++) {
        float pct = 100.0f * ts[i].ulRunTimeCounter / window;
        if (!strncmp(ts[i].pcTaskName, "IDLE", 4)) { idle[ts[i].pcTaskName[4] == '1'] = pct; continue; }
        int core = ts[i].xCoreID > 1 ? -1 : ts[i].xCoreID;
        len += snprintf(line + len, sizeof(line) - len, " %s(c%d p%u) %.1f%% %uB |",
                        ts[i].pcTaskName, core, (unsigned)ts[i].uxCurrentPriority, pct,
                        (unsigned)ts[i].usStackHighWaterMark);
        if (len > 130 || i == n - 1) { ESP_LOGI(TAG, "tasks:%s", line); len = 0; line[0] = 0; }
    }
    if (len) ESP_LOGI(TAG, "tasks:%s", line);
    ESP_LOGI(TAG, "cpu: core0 %.0f%% busy, core1 %.0f%% busy (window %lu s)",
             100 - idle[0], 100 - idle[1], (unsigned long)(window / 1000000));
}

static void diag_task(void *arg)
{
    int period = (int)(intptr_t)arg;
    size_t int_lg_min = SIZE_MAX, dma_lg_min = SIZE_MAX;
    // After the USB Serial/JTAG port is back: it re-enumerates at reset, and a monitor on the PC misses the first
    // ~2.5 s of the log (the boot info was always lost)
    // (at 4 s of uptime, whenever diag_start() was called: an app that starts it late doesn't wait 4 s more)
    int64_t up_ms = esp_timer_get_time() / 1000;
    if (up_ms < 4000) vTaskDelay(pdMS_TO_TICKS(4000 - up_ms));
    startup_info();
#if CONFIG_FORGE_DIAG_TASK_STATS
    report_tasks();                                               // baseline
#endif
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // Largest free blocks: sampled every second, worst value reported (fragmentation)
        size_t a = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t b = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (a < int_lg_min) int_lg_min = a;
        if (b < dma_lg_min) dma_lg_min = b;
        if (tick % period) continue;

        // PSRAM's largest block walks the whole PSRAM heap with interrupts off (~2 ms with 32 MB): optional (L201)
        char ps_lg[12] = "-";
#if CONFIG_FORGE_DIAG_PSRAM_LARGEST
        snprintf(ps_lg, sizeof(ps_lg), "%u", KB(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#endif
        ESP_LOGI(TAG, "heap: internal %u KB free (min ever %u, largest block now %u / worst %u) | "
                 "DMA %u KB (largest now %u / worst %u) | PSRAM %u KB free (min ever %u, largest %s) | "
                 "failed allocs %lu, LVGL in internal RAM %lu",
                 KB(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 KB(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), KB(a), KB(int_lg_min),
                 KB(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)), KB(b), KB(dma_lg_min),
                 KB(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), KB(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
                 ps_lg, (unsigned long)failed_allocs, (unsigned long)lvgl_mem_fallbacks());
        int_lg_min = dma_lg_min = SIZE_MAX;

        for (int i = 0; i < nhooks; i++) hooks[i]();
#if CONFIG_FORGE_DIAG_TASK_STATS
        report_tasks();                                           // (Kconfig: interrupts off ~6 ms on a P4)
#endif
    }
}

void diag_mark(const char *stage)
{
    ESP_LOGI(TAG, "mark %-14s internal %u KB free (largest %u), DMA %u KB, PSRAM %u KB", stage,
             KB(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             KB(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             KB(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
             KB(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void diag_start(int period_s)
{
    heap_caps_register_failed_alloc_callback(alloc_failed);
    xTaskCreatePinnedToCore(diag_task, "diag", 4096, (void *)(intptr_t)period_s, 1, NULL, 0);
}
