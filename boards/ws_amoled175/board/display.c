// CO5300 466x466 AMOLED over QSPI (Waveshare ESP32-S3-Touch-AMOLED-1.75) + LVGL 9 port (see board.h)
#include "board.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "display";

#define LCD_HOST   SPI2_HOST
#define PIN_CS     12
#define PIN_CLK    38
#define PIN_D0     4
#define PIN_D1     5
#define PIN_D2     6
#define PIN_D3     7
#define PIN_RST    39
#define X_GAP      6            // the panel's visible area starts at column 6
// 32 rows per band (two bands, internal DMA RAM): 2 x 30 KB. Bigger bands left too little internal DMA memory for
// TLS (hardware AES) and the settings page arrived truncated.
#define BUF_LINES  32

/* ---- diagnostics: lock contention and frame timing ---- */
static display_stats_t st, tst;          // st: the diag line; tst: the test console's "fps" (its own, so a diag
                                          // report in the middle of a measurement doesn't reset it)

static void count_frame(uint32_t render_us)
{
    display_stats_t *s[2] = {&st, &tst};
    for (int i = 0; i < 2; i++) {
        s[i]->frames++;
        s[i]->render_us += render_us;
        if (render_us > s[i]->render_max_us) s[i]->render_max_us = render_us;
    }
}

// Back-to-back frames (an animation): the interval. test = false leaves it out of the test console's "fps" (only):
// the gap from an LVGL redraw to a move's first frame (slide.c). It is the move's start, not a stall in it (drags log
// their start themselves), and an unrelated redraw just before a move made it look like one: weather_amoled's update
// check redrew a screen 247 ms before its harness's scroll (v1.12.3-rc.1, "REGRESSION" 247 ms > 120).
static bool last_raw;                  // the last frame was a move's (display_raw_frame), not LVGL's
static int64_t tst_t0;                 // the test console's last "fps reset"

static void count_anim(uint32_t gap_us, bool test, bool raw)
{
    display_stats_t *s[2] = {&st, &tst};
    for (int i = 0; i < (test ? 2 : 1); i++) {
        s[i]->anim_frames++;
        s[i]->anim_us += gap_us;
        if (gap_us > s[i]->anim_gap_max_us) {
            s[i]->anim_gap_max_us = gap_us;
            s[i]->anim_gap_max_at_ms = (uint32_t)((esp_timer_get_time() - tst_t0) / 1000);
            snprintf(s[i]->anim_gap_max_kind, sizeof(s[i]->anim_gap_max_kind), "%s>%s", last_raw ? "move" : "lvgl",
                     raw ? "move" : "lvgl");
        }
    }
}

static TaskHandle_t lvgl_th;
static int lock_depth;                 // only touched by the mutex owner
static int64_t lock_t0, render_t0, last_render;
static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t lvgl_mux;
volatile int disp_phase;               // breadcrumb for the test console's "where"
volatile int raw_band;                 // the band a raw frame is sending (with disp_phase 1-6)
static int lvgl_inflight;              // LVGL band transfers not finished yet (its last band outlives the refresh)
static void *buf1, *buf2;              // LVGL's two band buffers (internal, DMA), also used by display_raw_frame()
static volatile bool raw_mode;         // a raw frame is being sent: transfers complete to raw_done, not to LVGL
static SemaphoreHandle_t raw_done;

#define CMD(c)  (((uint32_t)0x02 << 24) | ((uint32_t)(c) << 8))
#define PIXELS  (((uint32_t)0x32 << 24) | ((uint32_t)0x2C << 8))

static void lcd_cmd(uint8_t c, const uint8_t *d, size_t n)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, CMD(c), d, n));
}

typedef struct { uint8_t cmd; uint8_t data[4]; uint8_t len; uint16_t delay_ms; } init_cmd_t;

// Vendor init sequence from Waveshare's BSP
static const init_cmd_t init_cmds[] = {
    {0xFE, {0x20}, 1, 0}, {0x19, {0x10}, 1, 0}, {0x1C, {0xA0}, 1, 0},
    {0xFE, {0x00}, 1, 0}, {0xC4, {0x80}, 1, 0}, {0x3A, {0x55}, 1, 0},
    {0x35, {0x00}, 1, 0}, {0x53, {0x20}, 1, 0}, {0x51, {0xFF}, 1, 0},
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, {0}, 0, 600},
    {0x29, {0}, 0, 0},
};

static bool on_trans_done(esp_lcd_panel_io_handle_t h, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    if (__atomic_load_n(&lvgl_inflight, __ATOMIC_ACQUIRE) > 0) {
        __atomic_fetch_sub(&lvgl_inflight, 1, __ATOMIC_ACQ_REL);
        lv_display_flush_ready((lv_display_t *)ctx);
        return false;
    }
    if (raw_mode) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(raw_done, &woken);
        return woken == pdTRUE;
    }
    return false;
}

/* A whole frame without LVGL (forge_lvgl's slide.c): bands of BUF_LINES rows, filled by the caller into one buffer
 * while the other is being sent. The caller holds the display lock, so LVGL isn't using the buffers. */
static bool raw_wait(void)
{
    if (xSemaphoreTake(raw_done, pdMS_TO_TICKS(200)) == pdTRUE) return true;
    ESP_LOGE(TAG, "raw frame: a band transfer did not finish");   // never hang the display (lock held)
    return false;
}

void display_raw_frame(display_fill_cb_t fill, void *user)
{
    const int rows = BUF_LINES;
    disp_phase = 1;
    for (int i = 0; i < 100 && __atomic_load_n(&lvgl_inflight, __ATOMIC_ACQUIRE) > 0; i++) vTaskDelay(1);   // LVGL's last band still going out
    while (xSemaphoreTake(raw_done, 0) == pdTRUE) {}                     // no stale tokens
    raw_mode = true;
    void *bufs[2] = {buf1, buf2};
    int inflight = 0;
    for (int y = 0, k = 0; y < DISP_H; y += rows, k ^= 1) {
        int n = DISP_H - y < rows ? DISP_H - y : rows;
        raw_band = y;
        disp_phase = 2;
        fill(y, n, bufs[k], user);                        // while the previous band is still going out
        disp_phase = 3;
        // No esp_lcd call while a transfer is in flight: tx_param/tx_color take the bus and then wait for the queued
        // transfer, and called during one they hung for good, a few frames in (weather_amoled v1.11.0, breadcrumbs:
        // phase 3). The same rule holds for any esp_lcd call from outside LVGL (display_brightness() waits too).
        if (inflight) { if (!raw_wait()) break; inflight--; }
        int cx1 = X_GAP, cx2 = DISP_W - 1 + X_GAP, y2 = y + n - 1;
        uint8_t col[4] = {cx1 >> 8, cx1 & 0xFF, cx2 >> 8, cx2 & 0xFF};
        uint8_t row[4] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        lcd_cmd(0x2A, col, 4);
        lcd_cmd(0x2B, row, 4);
        disp_phase = 4;
        esp_lcd_panel_io_tx_color(io, PIXELS, bufs[k], DISP_W * n * 2);
        disp_phase = 5;
        inflight++;
        st.pixels += DISP_W * n;
        tst.pixels += DISP_W * n;
    }
    disp_phase = 6;
    while (inflight-- > 0) if (!raw_wait()) break;
    disp_phase = 0;
    raw_mode = false;
    count_frame(0);                                       // counted like LVGL frames (fps in the test console)
    int64_t now = esp_timer_get_time();
    if (last_render && now - last_render < 250000) count_anim(now - last_render, last_raw, true);
    last_render = now;
    last_raw = true;
}

// A part of the screen (board.h): bands sized for the area's width, bottom up when asked (rows moved down in place
// need the lower rows first). From weather_amoled's display.c (its list scrolls, v1.14).
void display_raw_area(int x0, int y0, int x1, int y1, bool bottom_up, display_area_fill_cb_t fill, void *user)
{
    x0 &= ~1; y0 &= ~1; x1 |= 1; y1 |= 1;                 // the CO5300's even start / odd end (as the rounder)
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DISP_W - 1) x1 = DISP_W - 1;
    if (y1 > DISP_H - 1) y1 = DISP_H - 1;
    const int w = x1 - x0 + 1, rows = (DISP_W * BUF_LINES / w) & ~1;   // a narrow area: more rows per band
    disp_phase = 1;
    for (int i = 0; i < 100 && __atomic_load_n(&lvgl_inflight, __ATOMIC_ACQUIRE) > 0; i++) vTaskDelay(1);
    while (xSemaphoreTake(raw_done, 0) == pdTRUE) {}                     // no stale tokens
    raw_mode = true;
    void *bufs[2] = {buf1, buf2};
    int inflight = 0;
    const int last = y0 + (y1 - y0) / rows * rows;       // first row of the last band
    for (int y = bottom_up ? last : y0, k = 0; bottom_up ? y >= y0 : y <= y1; y += bottom_up ? -rows : rows, k ^= 1) {
        int n = y1 + 1 - y < rows ? y1 + 1 - y : rows;
        raw_band = y;
        disp_phase = 2;
        fill(x0, w, y, n, bufs[k], user);                 // while the previous band is still going out
        disp_phase = 3;
        if (inflight) { if (!raw_wait()) break; inflight--; }   // no esp_lcd call with a transfer in flight
        int cx1 = x0 + X_GAP, cx2 = x1 + X_GAP, y2 = y + n - 1;
        uint8_t col[4] = {cx1 >> 8, cx1 & 0xFF, cx2 >> 8, cx2 & 0xFF};
        uint8_t row[4] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        lcd_cmd(0x2A, col, 4);
        lcd_cmd(0x2B, row, 4);
        disp_phase = 4;
        esp_lcd_panel_io_tx_color(io, PIXELS, bufs[k], w * n * 2);
        disp_phase = 5;
        inflight++;
        st.pixels += w * n;
        tst.pixels += w * n;
    }
    disp_phase = 6;
    while (inflight-- > 0) if (!raw_wait()) break;
    disp_phase = 0;
    raw_mode = false;
    count_frame(0);                                       // counted like LVGL frames (fps in the test console)
    int64_t now = esp_timer_get_time();
    if (last_render && now - last_render < 250000) count_anim(now - last_render, last_raw, true);
    last_render = now;
    last_raw = true;
}

static display_flush_hook_t flush_hook;
void display_set_flush_hook(display_flush_hook_t hook) { flush_hook = hook; }

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    int x1 = a->x1 + X_GAP, x2 = a->x2 + X_GAP;
    uint8_t col[4] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    uint8_t row[4] = {a->y1 >> 8, a->y1 & 0xFF, a->y2 >> 8, a->y2 & 0xFF};
    lcd_cmd(0x2A, col, 4);
    lcd_cmd(0x2B, row, 4);
    uint32_t n = lv_area_get_size(a);
    st.pixels += n;
    tst.pixels += n;
    if (flush_hook) flush_hook(a, px);                    // before the swap: LVGL's own byte order
    lv_draw_sw_rgb565_swap(px, n);
    __atomic_fetch_add(&lvgl_inflight, 1, __ATOMIC_ACQ_REL);   // the transfer-done interrupt may run on the other core
    esp_lcd_panel_io_tx_color(io, PIXELS, px, n * 2);
}

// CO5300 needs even start / odd end coordinates
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1; a->y1 &= ~1;
    a->x2 |= 1;  a->y2 |= 1;
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool display_lock(int timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    bool ok = xSemaphoreTakeRecursive(lvgl_mux, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (ok && ++lock_depth == 1) {
        lock_t0 = esp_timer_get_time();
        if (xTaskGetCurrentTaskHandle() == lvgl_th && lock_t0 - t0 > st.lvgl_wait_max_us)
            st.lvgl_wait_max_us = lock_t0 - t0;          // LVGL blocked by another task
    }
    return ok;
}

void display_unlock(void)
{
    if (--lock_depth == 0 && xTaskGetCurrentTaskHandle() != lvgl_th) {
        int64_t held = esp_timer_get_time() - lock_t0;
        if (held > st.hold_max_us) {
            st.hold_max_us = held;
            strlcpy(st.hold_task, pcTaskGetName(NULL), sizeof(st.hold_task));
        }
    }
    xSemaphoreGiveRecursive(lvgl_mux);
}

static void render_evt(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        if (last_render && now - last_render < 250000) count_anim(now - last_render, true, false);   // back-to-back = animation
        last_render = render_t0 = now;
        last_raw = false;
    } else {
        count_frame(now - render_t0);
    }
}

int display_lvgl_inflight(void) { return __atomic_load_n(&lvgl_inflight, __ATOMIC_ACQUIRE); }

void display_get_stats(display_stats_t *out, bool reset)
{
    display_lock(-1);
    *out = st;
    if (reset) memset(&st, 0, sizeof(st));
    display_unlock();
}

void display_get_test_stats(display_stats_t *out, bool reset)
{
    display_lock(-1);
    *out = tst;
    if (reset) { memset(&tst, 0, sizeof(tst)); tst_t0 = esp_timer_get_time(); }
    display_unlock();
}

/* The panel asleep while the screen is off (display_sleep: forge_presence's screen_off hook, through the app). Going
 * to sleep is two commands from the calling task (the presence task, core 0), as display_brightness(). Coming back is
 * split: the caller sends SLPOUT (one command, the same rule), and the LVGL task finishes on core 1 (wake_at): the
 * 120 ms the DCS asks for after SLPOUT, a whole frame into the panel's memory, then DISPON. A whole refresh from core 0
 * would be the cross-core race of lesson L81 (the transfer-done interrupt is on core 1). From MeteoBus v0.2.1. */
static volatile bool asleep;
static volatile int64_t wake_at;         // SLPOUT sent: DISPON after this time (esp_timer us), 0 = nothing pending
// Held while the panel is awake or waking: the CPU stays at 240 MHz even when idle (board.c configures 80..240 MHz).
// Released while it sleeps: an idle core drops to 80 MHz (a running task still gets 240, ESP-IDF's "rtos" locks).
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t cpu_lock;
#endif
static bool cpu_held;                    // (the lock counts acquires: one each way)

static void cpu_full_speed(bool on)
{
    if (on == cpu_held) return;
#if CONFIG_PM_ENABLE
    if (!cpu_lock && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "screen", &cpu_lock) != ESP_OK) return;
    if (on) esp_pm_lock_acquire(cpu_lock); else esp_pm_lock_release(cpu_lock);
#endif
    cpu_held = on;
}

static void wake_finish(void)            // LVGL task, lock held
{
    int64_t left = wake_at - esp_timer_get_time();
    if (left > 0) vTaskDelay(pdMS_TO_TICKS(left / 1000 + 1));
    lv_obj_invalidate(lv_screen_active());              // a whole frame into the panel's memory before it shows
    lv_refr_now(NULL);
    disp_phase = 7;
    for (int i = 0; i < 100 && __atomic_load_n(&lvgl_inflight, __ATOMIC_ACQUIRE) > 0; i++) vTaskDelay(1);
    disp_phase = 8;
    lcd_cmd(0x29, NULL, 0);                             // DISPON
    disp_phase = 0;
    wake_at = 0;
    asleep = false;
    ESP_LOGI(TAG, "panel awake");
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t wait = 10;
        if (display_lock(-1)) {
            if (wake_at) wake_finish();
            wait = lv_timer_handler();
            display_unlock();
        }
        // 1 ms minimum: enough to let other tasks run (at 60 fps a frame is 16.7 ms: a 5 ms minimum cost 30 %)
        if (wait < 1) wait = 1;
        if (wait > 50) wait = 50;
        if (asleep && wait < 100) wait = 100;   // the panel asleep: 10 rounds a second (the touch that wakes it is read here)
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

void display_sleep(bool off)
{
    // Called with the display lock held, from any task. As display_brightness(): no esp_lcd call while LVGL's last
    // band is still going out.
    if (off == asleep && !wake_at) return;
    disp_phase = 7;
    for (int i = 0; i < 100 && __atomic_load_n(&lvgl_inflight, __ATOMIC_ACQUIRE) > 0; i++) vTaskDelay(1);
    disp_phase = 8;
    if (off) {
        wake_at = 0;                                    // (a wake not finished yet: the panel goes back to sleep)
        lcd_cmd(0x28, NULL, 0);                         // DISPOFF
        lcd_cmd(0x10, NULL, 0);                         // SLPIN: the panel's oscillator and drivers stop
        asleep = true;
        cpu_full_speed(false);
        ESP_LOGI(TAG, "panel asleep");
    } else if (!wake_at) {
        cpu_full_speed(true);
        lcd_cmd(0x11, NULL, 0);                         // SLPOUT: the DCS minimum before the next command is 120 ms
        wake_at = esp_timer_get_time() + 120000;        // the LVGL task takes it from here (wake_finish)
    }
    disp_phase = 0;
}

bool display_asleep(void) { return asleep; }
bool display_cpu_full_speed(void) { return cpu_held; }   // the "screen" lock held (console "power")

void display_init(void)
{
    ESP_LOGI(TAG, "Init QSPI bus + CO5300");
    size_t buf_bytes = DISP_W * BUF_LINES * 2;

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_CLK, .data0_io_num = PIN_D0, .data1_io_num = PIN_D1,
        .data2_io_num = PIN_D2, .data3_io_num = PIN_D3,
        .max_transfer_sz = buf_bytes,
        // SPI interrupt on the LVGL task's core: "transfer done" (on_trans_done) then completes before the task runs
        // again. esp_lcd isn't thread-safe: on core 0 the task could reach esp_lcd while the driver was still
        // finishing that transfer on the other core, and the display hung for good (weather_amoled v1.11.0).
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(DISP_W, DISP_H);

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS, .dc_gpio_num = -1, .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000, .trans_queue_depth = 10,   // 40 MHz: 22 ms a frame, 80 MHz: 11 ms
        .on_color_trans_done = on_trans_done, .user_ctx = disp,
        .lcd_cmd_bits = 32, .lcd_param_bits = 8,
        .flags = { .quad_mode = true },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    gpio_config_t rst = { .pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(150));

    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
        lcd_cmd(init_cmds[i].cmd, init_cmds[i].len ? init_cmds[i].data : NULL, init_cmds[i].len);
        if (init_cmds[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }

    void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1 && b2);
    buf1 = b1; buf2 = b2;
    raw_done = xSemaphoreCreateCounting(2, 0);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, render_evt, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, render_evt, LV_EVENT_RENDER_READY, NULL);

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, &lvgl_th, 1);
    cpu_full_speed(true);                                 // the panel is awake: 240 MHz even when idle (display_sleep)
    ESP_LOGI(TAG, "Display ready (%dx%d)", DISP_W, DISP_H);
}

void display_brightness(uint8_t level)
{
    // Called with the display lock held, from any task. LVGL lets go of the lock with its last band still on the bus,
    // and an esp_lcd call during a transfer can hang for good: wait for that band first (any esp_lcd call from
    // outside LVGL must).
    disp_phase = 7;
    for (int i = 0; i < 100 && __atomic_load_n(&lvgl_inflight, __ATOMIC_ACQUIRE) > 0; i++) vTaskDelay(1);
    disp_phase = 8;
    lcd_cmd(0x51, &level, 1);
    disp_phase = 0;
}
