// espforge starter app: the framework's start-up order, Wi-Fi setup paths and settings route, with two screens.
// A new project keeps this skeleton and replaces the screens (ui.c) and the work loop at the end.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "board.h"
#include "board_audio.h"
#include "imu.h"
#include "diag.h"
#include "testcon.h"
#include "net.h"
#include "web.h"
#include "ota.h"
#include "presence.h"
#include "app_text.h"
#include "ui.h"

static const char *TAG = "app";
#define BOOT_BTN GPIO_NUM_0

extern const uint8_t page_start[] asm("_binary_index_html_start");
extern const uint8_t page_end[]   asm("_binary_index_html_end");

// Holding BOOT while pressing RESET enters download mode, so BOOT is read about a second after start-up:
// held for ~1 s then = forget the saved Wi-Fi
static bool boot_button_held(void)
{
    gpio_config_t c = { .pin_bit_mask = 1ULL << BOOT_BTN, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&c);
    for (int i = 0; i < 20; i++) {
        if (gpio_get_level(BOOT_BTN)) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

// cJSON's trees in PSRAM: thousands of small nodes went to internal RAM, the scarce one
static void *json_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

// POST /api/settings {"lang":"en"|"fr"}: the app's own settings (docs/PROTOCOL.md §4)
static esp_err_t settings_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    const char *lang = cJSON_GetStringValue(cJSON_GetObjectItem(j, "lang"));
    bool ok = lang && !strcmp(i18n_code(i18n_from_code(lang)), lang);   // one of the app's languages
    if (ok && i18n_from_code(lang) != i18n_lang()) {
        i18n_set(i18n_from_code(lang));
        ok = i18n_save();
        ui_texts_changed();
        ESP_LOGI(TAG, "language %s", lang);
    }
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad settings");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Screen dimming (forge_presence) on this board's microphones, motion sensor and touch
static bool mic_open(void) { return board_mic_open(30); }        // 30 dB: a room's background noise
static bool accel_open(void) { return imu_init(board_i2c_bus()); }
static void set_brightness(int pct)
{
    display_lock(-1);
    display_brightness((uint8_t)(pct * 255 / 100));
    display_unlock();
}
// A real off: the panel asleep (DISPOFF + SLPIN) once the fade down reached 0, awake again (SLPOUT, then the LVGL task
// sends a whole frame and DISPON) before the fade up; the CPU may idle at 80 MHz meanwhile (board.h display_sleep)
static void screen_off(bool off)
{
    display_lock(-1);
    display_sleep(off);
    display_unlock();
}
static const presence_hooks_t presence_hooks = {
    .mic_open = mic_open, .mic_read = board_mic_read, .accel_open = accel_open, .accel_read = imu_read,
    .touch_idle_ms = touch_idle_ms, .set_brightness = set_brightness, .screen_off = screen_off,
};

static const web_route_t app_routes[] = {
    { "/api/settings", HTTP_POST, settings_post, .keyed = true },
};

// The saved network can't be reached at start-up. Setup opens by itself for AUTO_SETUP_S, then the device just
// keeps trying the saved network; a long-press still opens setup (a setup network left open for hours is a way in
// for anyone nearby). A rule with a time window needs a test that crosses it: harness "wifi offline-boot-short".
#define AUTO_SETUP_S (15 * 60)
static void offline_setup(const char *ssid)
{
    char note[160], body[160], still[160];
    snprintf(note, sizeof(note), tr(T_CANT_REACH), ssid);
    snprintf(body, sizeof(body), tr(T_CONNECTING), ssid);
    snprintf(still, sizeof(still), tr(T_STILL_TRYING), ssid);
    int64_t until = esp_timer_get_time() + (net_test_short_setup() ? 60 : AUTO_SETUP_S) * 1000000LL;
    bool gave_up = false;
    while (1) {
        bool by_itself = esp_timer_get_time() < until;
        bool opened = by_itself && !ui_wifi_setup_open();
        if (opened) ui_wifi_setup(note);
        while (ui_wifi_setup_open() && !net_is_connected()) {
            // The window ends while it is open: it closes, unless a phone is on it (one the user opened stays)
            if (opened && esp_timer_get_time() >= until && ui_wifi_setup_close()) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (net_is_connected()) break;
        if (!gave_up && esp_timer_get_time() >= until) {
            gave_up = true;
            ESP_LOGW(TAG, "Setup network: no longer opened by itself (long-press opens it); still trying \"%s\"", ssid);
        }
        ui_message("Wi-Fi", gave_up ? still : body);
        if (net_wait_connected(30000)) break;
        if (!gave_up) ESP_LOGW(TAG, "Still can't reach \"%s\", offering the setup network again", ssid);
    }
    ESP_LOGI(TAG, "Saved network is back");
    ui_wifi_setup_end();
    ui_message("Wi-Fi", tr(T_CONNECTED));
    for (int i = 0; i < 120 && net_ap_clients() > 0; i++) vTaskDelay(pdMS_TO_TICKS(1000));   // let a phone finish
    net_setup_ap_stop();
}

// No saved network: the setup network and its captive portal until a phone sends one (then it restarts)
static void first_setup(void)
{
    net_start_portal();
    web_start();
    ui_wifi_setup(tr(T_FIRST_SETUP));
    diag_mark("app ready");
    while (1) vTaskDelay(portMAX_DELAY);
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting");
    setenv("TZ", CONFIG_APP_TZ, 1);   // the clock's time zone (menuconfig "Starter app"); SNTP sets UTC
    tzset();
    cJSON_InitHooks(&(cJSON_Hooks){ .malloc_fn = json_alloc, .free_fn = free });
    diag_mark("start");
    net_init();                 // NVS first: settings, the language, the saved network
    app_text_init();
    diag_start(60);             // "diag:" lines every 60 s (heap, tasks, display)
    board_init();               // display, LVGL, touch; registers fps/tap/swipe/screen with the test console
    diag_mark("board");
    web_set_page(page_start, page_end);
    web_add_routes(app_routes, sizeof(app_routes) / sizeof(app_routes[0]));
    presence_start(&presence_hooks);    // dims, then turns off, the screen in a quiet room (console: presence, wake)
    presence_web_routes();
    touch_set_press_filter(presence_touch);   // a touch on a dark screen only wakes it
    ota_start(ui_ota);          // logs "ota: Running ..."; checks once Wi-Fi is up; confirms a new image after 60 s
    ui_init();
    testcon_start();            // ready before Wi-Fi, so start-up itself can be tested
    ui_message("espforge", tr(T_STARTING));

    if (boot_button_held()) net_clear_creds();
    char ssid[NET_SSID_MAX + 1] = {0}, pass[NET_PASS_MAX + 1] = {0};
    if (!net_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "No saved Wi-Fi, starting setup portal");
        first_setup();
    }

    char body[160];
    snprintf(body, sizeof(body), tr(T_CONNECTING), ssid);
    ui_message("Wi-Fi", body);
    net_begin(ssid, pass);
    web_start();                // up early, so a long-press can offer the setup page right away
    if (!net_wait(30000)) {
        ESP_LOGW(TAG, "Wi-Fi connect failed, offering the setup network");
        offline_setup(ssid);
    }
    diag_mark("wifi up");
    ui_home();
    diag_mark("app ready");     // forge.json ready_line: the harness starts its tests here

    // The app's work goes here. Waits must wake on requests (ulTaskNotifyTake), never a fixed vTaskDelay that makes
    // the device ignore the user for its length.
    while (1) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
