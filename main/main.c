// ColorMesh: three boards share colours over Bluetooth LE (docs/RESEARCH-BLE.md). No Wi-Fi: configuration and
// updates come over USB (the Pages site) or, in dev mode, from a peer.
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "board.h"
#include "diag.h"
#include "nvs_util.h"
#include "testcon.h"
#include "app_text.h"
#include "config.h"
#include "mesh.h"
#include "console.h"
#include "ui.h"

static const char *TAG = "app";

// cJSON's trees in PSRAM: thousands of small nodes went to internal RAM, the scarce one
static void *json_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting");
    cJSON_InitHooks(&(cJSON_Hooks){ .malloc_fn = json_alloc, .free_fn = free });
    diag_mark("start");
    nvs_init();                 // NVS first: the language, the app's settings, NimBLE's PHY calibration
    app_text_init();
    diag_start(60);             // "diag:" lines every 60 s (heap, tasks, display)
    board_init();               // display, LVGL, touch; registers fps/tap/swipe/screen with the test console
    diag_mark("board");
    config_load();              // colour, dev mode, name, brightness (applied here)
    ui_init();
    mesh_start();               // NimBLE; reads config_get(), so after config_load()
    console_init();             // the app's test console commands
    testcon_start();
    if (config_get()->has_color) ui_show_main();   // the ring is one swipe away; a fresh board starts on it
    else ui_show_pick();
    diag_mark("app ready");     // forge.json ready_line: the harness starts its tests here

    // The app's work runs in its own tasks. Waits must wake on requests (ulTaskNotifyTake), never a fixed
    // vTaskDelay that makes the device ignore the user for its length.
    while (1) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
