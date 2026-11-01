// Test console: the USB reader task and the built-in commands (see testcon.h, docs/PROTOCOL.md §2)
#include "testcon.h"
#include "esp_attr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "diag.h"

static const char *TAG = "test";

#define MAX_WHERE 8
static testcon_where_fn_t where_fns[MAX_WHERE];
static int nwhere;

void testcon_add_where(testcon_where_fn_t fn)
{
    if (nwhere < MAX_WHERE) where_fns[nwhere++] = fn;
}

// LVGL blocks put in internal RAM because PSRAM was full (forge_lvgl's lvgl_mem.c); 0 in a build without it
__attribute__((weak)) uint32_t lvgl_mem_fallbacks(void) { return 0; }

static void cmd_ping(int argc, char **argv) { ESP_LOGI(TAG, "pong %s", esp_app_get_description()->version); }

static void cmd_help(int argc, char **argv)
{
    EXT_RAM_BSS_ATTR static char out[600];                 // static: the console task's stack is small (PSRAM: L185)
    testcon_help(out, sizeof(out));
    ESP_LOGI(TAG, "commands: %s", out);
}

static void cmd_heap(int argc, char **argv)
{
    ESP_LOGI(TAG, "heap internal=%u min=%u largest=%u psram=%u psram_min=%u uptime_s=%lld failed_allocs=%lu lvgl_fallbacks=%lu",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024,
             esp_timer_get_time() / 1000000, (unsigned long)diag_failed_allocs(), (unsigned long)lvgl_mem_fallbacks());
}

// No lock of any kind: answers even when the display or another task is stuck (the breadcrumbs say where)
static void cmd_where(int argc, char **argv)
{
    EXT_RAM_BSS_ATTR static char out[200];
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < nwhere && len < sizeof(out); i++) {
        where_fns[i](out + len, sizeof(out) - len);
        len = strlen(out);
    }
    TaskHandle_t h = xTaskGetHandle("lvgl");
    ESP_LOGI(TAG, "where%s task_lvgl=%d", out, h ? (int)eTaskGetState(h) : -1);
}

// Copy speeds that bound full-screen drawing: PSRAM <-> internal in display-band sized pieces, PSRAM -> PSRAM
static void cmd_memspeed(int argc, char **argv)
{
    const size_t big = 466 * 466 * 2, band = 466 * 16 * 2;  // one 466x466 RGB565 frame, 16-row bands
    uint8_t *ps = heap_caps_malloc(big, MALLOC_CAP_SPIRAM), *ps2 = heap_caps_malloc(big, MALLOC_CAP_SPIRAM);
    uint8_t *in = heap_caps_malloc(band, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!ps || !ps2 || !in) { ESP_LOGW(TAG, "error memspeed: no memory"); goto out; }
    memset(ps, 0x55, big); memset(ps2, 0x55, big);
    int64_t t0 = esp_timer_get_time();
    for (size_t o = 0; o + band <= big; o += band) memcpy(in, ps + o, band);
    int64_t t1 = esp_timer_get_time();
    for (size_t o = 0; o + band <= big; o += band) memcpy(ps2 + o, in, band);
    int64_t t2 = esp_timer_get_time();
    memcpy(ps2, ps, big);
    int64_t t3 = esp_timer_get_time();
    ESP_LOGI(TAG, "memspeed bytes=%u psram_to_internal_ms=%.1f internal_to_psram_ms=%.1f psram_to_psram_ms=%.1f",
             (unsigned)big, (t1 - t0) / 1000.0f, (t2 - t1) / 1000.0f, (t3 - t2) / 1000.0f);
out:
    free(ps); free(ps2); free(in);
}

static void cmd_reboot(int argc, char **argv)
{
    ESP_LOGI(TAG, "ok restarting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void testcon_task(void *arg)
{
    char line[120];
    int n = 0;
    uint8_t ch;
    while (1) {
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[n] = 0;
            n = 0;
            testcon_dispatch(line);
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = ch;
        }
    }
}

void testcon_start(void)
{
    testcon_register("ping", "ping", cmd_ping);
    testcon_register("help", "help", cmd_help);
    testcon_register("heap", "heap", cmd_heap);
    testcon_register("where", "where", cmd_where);
    testcon_register("memspeed", "memspeed", cmd_memspeed);
    testcon_register("reboot", "reboot", cmd_reboot);
    usb_serial_jtag_driver_config_t cfg = { .rx_buffer_size = 256, .tx_buffer_size = 256 };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) { ESP_LOGW(TAG, "USB serial driver not installed"); return; }
    // Stack in internal RAM: commands read and write flash (saved network, Wi-Fi config), and a task with a PSRAM
    // stack can't run while flash is busy ("wifi online" reset the board when it had one). Check "testcon" in the
    // diag tasks line after adding commands: 3 KB left 568 B spare in weather_amoled.
    xTaskCreatePinnedToCore(testcon_task, "testcon", 4096, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "console ready on USB (send 'help')");
}
