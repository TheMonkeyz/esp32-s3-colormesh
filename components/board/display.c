// CO5300 466x466 AMOLED over QSPI (Waveshare ESP32-S3-Touch-AMOLED-1.75) + LVGL 9 port (see board.h)
//
// Tear-free frames: LVGL renders (DIRECT mode) into one full frame in PSRAM, and the frame goes to the panel only once
// it is complete, in bands of BUF_LINES rows copied into two internal DMA buffers, started on the panel's Tearing
// Effect signal (TE, GPIO13) so the write runs ahead of the panel's scan. Before: PARTIAL mode in two 32-line bands,
// each sent as soon as rendered; a full redraw (65-85 ms) spanned four or five panel refreshes, which showed part old,
// part new frame ("tearing when I tap the screens quickly", 2026-10-31). Moves drawn as pictures (display_raw_frame)
// start on TE too.
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG       // the first frames' timings at DEBUG (esp_log_level_set to see them)
#include "board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "hal/gpio_ll.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "testcon.h"

static const char *TAG = "display";

#define LCD_HOST   SPI2_HOST
#define PIN_CS     12
#define PIN_CLK    38
#define PIN_D0     4
#define PIN_D1     5
#define PIN_D2     6
#define PIN_D3     7
#define PIN_RST    39
#define PIN_TE     13           // LCD_TE (the schematic's pin table); the panel pulses it once per refresh (0x35)
#define X_GAP      6            // the panel's visible area starts at column 6
// 32 rows per band (two bands, internal DMA RAM): 2 x 30 KB. Bigger bands left too little internal DMA memory for
// TLS (hardware AES) and the settings page arrived truncated.
#define BUF_LINES  32
#define FB_STRIDE  (DISP_W * 2)
#define TE_TIMEOUT_MS 25        // a frame never waits longer for TE (a missing pulse must not stall the display)

/* ---- diagnostics: lock contention and frame timing ---- */
static display_stats_t st, tst;          // st: the diag line; tst: the test console's "fps" (its own, so a diag
                                          // report in the middle of a measurement doesn't reset it)
static display_stats_t sum;              // the 60 s "display: frames ..." summary

static void count_frame(uint32_t render_us)
{
    display_stats_t *s[3] = {&st, &tst, &sum};
    for (int i = 0; i < 3; i++) {
        s[i]->frames++;
        s[i]->render_us += render_us;
        if (render_us > s[i]->render_max_us) s[i]->render_max_us = render_us;
    }
}

// One push to the panel (an LVGL frame or a raw one): its time on the bus, its wait for TE, how long after the TE edge
// its first band started
static void count_push(uint32_t push_us, uint32_t wait_us, uint32_t lag_us, bool timeout, bool chased)
{
    display_stats_t *s[3] = {&st, &tst, &sum};
    for (int i = 0; i < 3; i++) {
        s[i]->pushes++;
        s[i]->push_us += push_us;
        if (push_us > s[i]->push_max_us) s[i]->push_max_us = push_us;
        s[i]->te_wait_us += wait_us;
        if (wait_us > s[i]->te_wait_max_us) s[i]->te_wait_max_us = wait_us;
        if (timeout) s[i]->te_timeouts++;
        else if (chased) s[i]->te_chased++;
        else if (lag_us > s[i]->te_lag_max_us) s[i]->te_lag_max_us = lag_us;
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
volatile int raw_band;                 // the band a push is sending (with disp_phase 1-6, 9)
static int band_inflight;              // band transfers not finished yet (always 0 between pushes)
static void *buf1, *buf2;              // the two band buffers (internal, DMA) every push goes through
static uint8_t *fb;                    // LVGL's frame (PSRAM, RGB565 as LVGL draws it)
static SemaphoreHandle_t band_done;

#define CMD(c)  (((uint32_t)0x02 << 24) | ((uint32_t)(c) << 8))
#define PIXELS  (((uint32_t)0x32 << 24) | ((uint32_t)0x2C << 8))

static void lcd_cmd(uint8_t c, const uint8_t *d, size_t n)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, CMD(c), d, n));
}

typedef struct { uint8_t cmd; uint8_t data[4]; uint8_t len; uint16_t delay_ms; } init_cmd_t;

// Vendor init sequence from Waveshare's BSP (0x35 0x00: the TE output on, V-blank only)
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
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(band_done, &woken);
    return woken == pdTRUE;
}

/* ---------- Tearing Effect ----------
 * The panel raises TE at the start of each vertical blank (measured on board A, 2026-10-31: 59 Hz, period 16.85 ms,
 * high 0.63 ms), then scans its memory top to bottom in the remaining ~16.2 ms. A push started on the rising edge
 * (0.01 ms after it) writes rows faster than the scan reads them (a whole frame in 13.9-15.9 ms), so the scan never
 * overtakes the write: each refresh shows one whole frame. The lead is smallest at the top (the 0.6 ms blank): a stall
 * of the LVGL task longer than that in the first bands (a flash write with the cache off) can still tear once. */
static SemaphoreHandle_t te_sem;        // given on every rising edge
static volatile uint32_t te_rise_us;    // the last rising edge (esp_timer, low 32 bits: one load across cores)
static volatile uint32_t te_rises, te_high_us, te_period_us;
static int te_state;                    // 0 = being checked (wait, bounded), 1 = pulsing, -1 = no TE: never wait
static bool te_sync_lvgl = true, te_sync_raw = true;   // "te lvgl|raw on|off" in the test console
static bool te_chase_lvgl = true;       // "te chase on|off": LVGL frames may start behind the scan (te_sync)
static int te_log_n;                    // "te log N": the next N pushes logged at INFO

static void IRAM_ATTR te_isr(void *arg)
{
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (gpio_ll_get_level(&GPIO, PIN_TE)) {
        if (te_rise_us) te_period_us = now - te_rise_us;
        te_rise_us = now;
        te_rises++;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(te_sem, &woken);
        if (woken) portYIELD_FROM_ISR();
    } else if (te_rise_us) {
        te_high_us = now - te_rise_us;
    }
}

/* Before a push of rows y1..y2 (`rows` of them, gaps between runs left out): when to start.
 * - edge: on the next TE rising edge (at most TE_TIMEOUT_MS); just past one (TE still high, under 0.3 ms): now. The
 *   write then runs ahead of the scan all the way down.
 * - chase (LVGL frames, and a move's first frame): also now, or after a short wait, when the scan is far enough down
 *   that the write, starting behind it, can never catch it up in this refresh: the rows change only after this
 *   refresh read them, and the next refresh shows them whole (the same refresh an edge start would reach, sooner for
 *   rows near the top), without waiting up to 17 ms for the edge. Far enough: the write at most at the bus's speed (a
 *   full frame in 10.9 ms at 80 MHz, no push is faster), the scan at the measured period minus the blank, so the last
 *   row y2 must be scanned before the write could reach it: from = blank + y2 * scan row time - rows * fastest write
 *   row time + 0.5 ms (a full frame: ~6.5 ms after the edge). The write then finishes before the next refresh reaches
 *   its rows (it is faster than the scan, and starts at least 0.5 ms before the next edge, else: the edge). Also now,
 *   for rows starting below the scan by more than 1 ms of scan (the write starts ahead and outruns it).
 * *lag = from the last edge to the start. */
#define BUS_FRAME_US 10900              // 466 x 466 x 2 bytes at 40 MB/s (QSPI, 80 MHz): the fastest possible push

static void te_sync(bool on, bool chase, int y1, int y2, int rows, uint32_t *wait, uint32_t *lag, bool *timeout,
                    bool *chased)
{
    int64_t t0 = esp_timer_get_time();
    *wait = *lag = 0;
    *timeout = *chased = false;
    if (!on || te_state < 0) return;
    disp_phase = 9;
    uint32_t p = te_period_us, b = te_high_us, since = (uint32_t)t0 - te_rise_us;
    bool in_blank = te_state > 0 && gpio_get_level(PIN_TE) && since < b && since < 300;
    int64_t at = -1;                                     // chasing: start at this time
    if (!in_blank && chase && te_state > 0 && p > 10000 && p < 25000 && b < p / 2 && since < p - 500) {
        float ls = (float)(p - b) / DISP_H, lw = (float)BUS_FRAME_US / DISP_H;   // us per row: scan, fastest write
        float from = b + y2 * ls - rows * lw + 500, ahead = b + y1 * ls - 1000;
        if (since <= ahead) at = t0;                                       // below the scan: ahead of it
        else if (from < p - 500) at = t0 + (since < from ? (int64_t)(from - since) : 0);   // full frame: <= 6.2 ms
    }
    if (in_blank) {
    } else if (at >= 0) {
        int64_t d = at - esp_timer_get_time();
        if (d > 1500) vTaskDelay(pdMS_TO_TICKS(d / 1000 - 1));            // the bulk asleep, the rest spun
        while (esp_timer_get_time() < at) {}
        *chased = true;
    } else {
        xSemaphoreTake(te_sem, 0);                                         // an edge already past: not that one
        *timeout = xSemaphoreTake(te_sem, pdMS_TO_TICKS(TE_TIMEOUT_MS)) != pdTRUE;
    }
    int64_t t1 = esp_timer_get_time();
    *wait = t1 - t0;
    *lag = *timeout ? 0 : (uint32_t)t1 - te_rise_us;
}

/* ---------- pushes: rows of a frame to the panel ----------
 * Runs of rows (each starting on an even row and ending on an odd one: the CO5300's rule), sent in bands of BUF_LINES
 * full-width rows; fill() writes a band into one DMA buffer while the other is on the bus. The first band is filled
 * before the wait for TE, so the bus starts at once on the edge. The caller holds the display lock. */
typedef struct { int16_t y1, y2; } run_t;

static bool band_wait(void)
{
    if (xSemaphoreTake(band_done, pdMS_TO_TICKS(200)) == pdTRUE) return true;
    ESP_LOGE(TAG, "push: a band transfer did not finish");   // never hang the display (lock held)
    return false;
}

static void push(const run_t *runs, int nruns, display_fill_cb_t fill, void *user, bool sync, bool chase, bool lvgl,
                 uint32_t render_us)
{
    if (nruns <= 0) return;
    disp_phase = 1;
    for (int i = 0; i < 100 && __atomic_load_n(&band_inflight, __ATOMIC_ACQUIRE) > 0; i++) vTaskDelay(1);
    while (xSemaphoreTake(band_done, 0) == pdTRUE) {}                    // no stale tokens
    void *bufs[2] = {buf1, buf2};
    int r = 0, y = runs[0].y1, k = 0, rows = 0;
    int n = runs[0].y2 - y + 1 < BUF_LINES ? runs[0].y2 - y + 1 : BUF_LINES;
    raw_band = y;
    disp_phase = 2;
    fill(y, n, bufs[k], user);
    int total = 0;
    for (int i = 0; i < nruns; i++) total += runs[i].y2 - runs[i].y1 + 1;
    uint32_t wait, lag;
    bool timeout, chased;
    te_sync(sync, chase, runs[0].y1, runs[nruns - 1].y2, total, &wait, &lag, &timeout, &chased);
    int64_t t0 = esp_timer_get_time(), ta;
    uint32_t us_fill = 0, us_wait = 0, us_cmd = 0;          // where a push's time goes ("te log")
    for (;;) {
        disp_phase = 3;
        ta = esp_timer_get_time();
        // No esp_lcd call while a transfer is in flight: tx_param/tx_color take the bus and then wait for the queued
        // transfer, and called during one they hung for good, a few frames in (weather_amoled v1.11.0, breadcrumbs:
        // phase 3). The same rule holds for any esp_lcd call from outside LVGL (display_brightness() waits too).
        if (band_inflight) { if (!band_wait()) break; __atomic_fetch_sub(&band_inflight, 1, __ATOMIC_ACQ_REL); }
        int64_t tb = esp_timer_get_time();
        us_wait += tb - ta;
        int cx1 = X_GAP, cx2 = DISP_W - 1 + X_GAP, y2 = y + n - 1;
        uint8_t col[4] = {cx1 >> 8, cx1 & 0xFF, cx2 >> 8, cx2 & 0xFF};
        uint8_t row[4] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        if (!rows) lcd_cmd(0x2A, col, 4);                                  // the columns stay: once a push
        lcd_cmd(0x2B, row, 4);
        disp_phase = 4;
        __atomic_fetch_add(&band_inflight, 1, __ATOMIC_ACQ_REL);
        esp_lcd_panel_io_tx_color(io, PIXELS, bufs[k], DISP_W * n * 2);
        disp_phase = 5;
        ta = esp_timer_get_time();
        us_cmd += ta - tb;
        rows += n;
        y += n;                                                            // the next band
        if (y > runs[r].y2) { if (++r >= nruns) break; y = runs[r].y1; }
        n = runs[r].y2 - y + 1 < BUF_LINES ? runs[r].y2 - y + 1 : BUF_LINES;
        k ^= 1;
        raw_band = y;
        disp_phase = 2;
        fill(y, n, bufs[k], user);                                         // while the previous band is going out
        us_fill += esp_timer_get_time() - ta;
    }
    disp_phase = 6;
    while (band_inflight > 0) { if (!band_wait()) break; __atomic_fetch_sub(&band_inflight, 1, __ATOMIC_ACQ_REL); }
    __atomic_store_n(&band_inflight, 0, __ATOMIC_RELEASE);
    disp_phase = 0;
    uint32_t push_us = esp_timer_get_time() - t0;
    st.pixels += (uint64_t)DISP_W * rows;
    tst.pixels += (uint64_t)DISP_W * rows;
    count_push(push_us, wait, lag, timeout, chased);
    static int first = 20;                                                 // the first frames after boot
    if (te_log_n > 0) {
        te_log_n--;
        ESP_LOGI(TAG, "frame %.1f + %.1f ms, te wait %.1f ms (%s, %d rows, edge +%.2f ms; fill %.1f, bus wait %.1f, "
                 "cmd %.1f ms)%s", render_us / 1000.0f, push_us / 1000.0f, wait / 1000.0f, lvgl ? "lvgl" : "raw", rows,
                 lag / 1000.0f, us_fill / 1000.0f, us_wait / 1000.0f, us_cmd / 1000.0f,
                 timeout ? " TE TIMEOUT" : chased ? " behind / ahead of the scan" : "");
    } else if (first > 0) {
        first--;
        ESP_LOGD(TAG, "frame %.1f + %.1f ms, te wait %.1f ms (%s, %d rows)", render_us / 1000.0f, push_us / 1000.0f,
                 wait / 1000.0f, lvgl ? "lvgl" : "raw", rows);
    }
}

/* A whole frame without LVGL (forge_lvgl's slide.c): fill() writes each band. The caller holds the display lock, so
 * LVGL isn't using the band buffers. */
void display_raw_frame(display_fill_cb_t fill, void *user)
{
    static const run_t all = {0, DISP_H - 1};
    static int64_t raw_end;                               // a move's first frame: none in the last 100 ms
    push(&all, 1, fill, user, te_sync_raw, esp_timer_get_time() - raw_end > 100000, false, 0);
    raw_end = esp_timer_get_time();
    count_frame(0);                                       // counted like LVGL frames (fps in the test console)
    int64_t now = esp_timer_get_time();
    if (last_render && now - last_render < 250000) count_anim(now - last_render, last_raw, true);
    last_render = now;
    last_raw = true;
}

static display_flush_hook_t flush_hook;
void display_set_flush_hook(display_flush_hook_t hook) { flush_hook = hook; }

// LVGL's frame -> a DMA band: RGB565 byte-swapped (panel order), two pixels at a time (rows are 4-byte aligned)
static void fb_fill(int y0, int n, void *dst, void *user)
{
    const uint32_t *s = (const uint32_t *)(fb + y0 * FB_STRIDE);
    uint32_t *d = dst;
    for (int i = 0, words = n * DISP_W / 2; i < words; i++) {
        uint32_t v = s[i];
        d[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
}

/* DIRECT mode: LVGL draws the dirty areas into fb, calling flush_cb once per area (the last flagged); the rows they
 * cover are collected and pushed on the last one, in order, after TE. Unchanged rows between two areas closer than a
 * band go too (the frame holds them as they are): fewer, fuller bands. */
static uint8_t dirty[DISP_H];
static int dirty_min = DISP_H, dirty_max = -1;
static int64_t flush_render_us;        // render time of the frame being flushed (RENDER_START to its last area)

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    int y1 = a->y1 < 0 ? 0 : a->y1, y2 = a->y2 > DISP_H - 1 ? DISP_H - 1 : a->y2;
    if (y1 <= y2) {
        memset(dirty + y1, 1, y2 - y1 + 1);
        if (y1 < dirty_min) dirty_min = y1;
        if (y2 > dirty_max) dirty_max = y2;
    }
    if (!lv_display_flush_is_last(disp) || dirty_max < 0) { lv_display_flush_ready(disp); return; }
    uint32_t render_us = esp_timer_get_time() - render_t0;
    run_t runs[DISP_H / 2 / 2 + 1];
    int nr = 0;
    for (int y = dirty_min & ~1; y <= dirty_max; ) {
        if (!dirty[y]) { y++; continue; }
        int e = y;
        while (e + 1 <= dirty_max && dirty[e + 1]) e++;
        e |= 1;                                           // odd end (the rounder already made it so)
        if (e > DISP_H - 1) e = DISP_H - 1;
        if (nr && y - runs[nr - 1].y2 - 1 < BUF_LINES) runs[nr - 1].y2 = e;
        else runs[nr++] = (run_t){ y & ~1, e };
        y = e + 1;
    }
    memset(dirty + dirty_min, 0, dirty_max - dirty_min + 1);
    dirty_min = DISP_H; dirty_max = -1;
    if (flush_hook)                                       // full-width rows, LVGL's own byte order
        for (int i = 0; i < nr; i++) {
            lv_area_t ra = { 0, runs[i].y1, DISP_W - 1, runs[i].y2 };
            flush_hook(&ra, fb + runs[i].y1 * FB_STRIDE);
        }
    push(runs, nr, fb_fill, NULL, te_sync_lvgl, te_chase_lvgl, true, render_us);
    flush_render_us = render_us;
    lv_display_flush_ready(disp);
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
        flush_render_us = -1;
    } else {
        // The render alone: up to the last area's flush (the push and its wait for TE are counted apart)
        count_frame(flush_render_us >= 0 ? flush_render_us : now - render_t0);
    }
}

int display_lvgl_inflight(void) { return __atomic_load_n(&band_inflight, __ATOMIC_ACQUIRE); }

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

static void log_summary(const display_stats_t *d)
{
    ESP_LOGI(TAG, "frames %lu (%lu pushes), render avg %.1f max %.1f ms, push avg %.1f max %.1f ms, te waits avg %.1f "
                  "max %.1f ms (edge to push max %.2f ms, %lu started off the edge), te timeouts %lu", (unsigned long)d->frames,
             (unsigned long)d->pushes, d->frames ? d->render_us / 1000.0f / d->frames : 0, d->render_max_us / 1000.0f,
             d->pushes ? d->push_us / 1000.0f / d->pushes : 0, d->push_max_us / 1000.0f,
             d->pushes ? d->te_wait_us / 1000.0f / d->pushes : 0, d->te_wait_max_us / 1000.0f,
             d->te_lag_max_us / 1000.0f, (unsigned long)d->te_chased, (unsigned long)d->te_timeouts);
}

static void lvgl_task(void *arg)
{
    int64_t next_sum = esp_timer_get_time() + 60000000LL;
    while (1) {
        uint32_t wait = 10;
        display_stats_t d = {0};
        bool report = false;
        if (display_lock(-1)) {
            wait = lv_timer_handler();
            if (esp_timer_get_time() >= next_sum) {
                next_sum += 60000000LL;
                d = sum;
                memset(&sum, 0, sizeof(sum));
                report = d.pushes > 0;
            }
            display_unlock();
        }
        if (report) log_summary(&d);
        // 1 ms minimum: enough to let other tasks run (at 60 fps a frame is 16.7 ms: a 5 ms minimum cost 30 %)
        if (wait < 1) wait = 1;
        if (wait > 50) wait = 50;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

/* TE check: rising edges counted over a short window at start-up (the 1 s count is logged by te_report). No pulse:
 * 0x35 sent again (0x00, V-blank only), then 0x01 (V and H blank); still none: never wait for it. */
static esp_timer_handle_t te_timer;
static uint32_t te_count0;

static void te_report(void *arg)
{
    uint32_t n = te_rises - te_count0;
    if (n >= 30 && n <= 120) {
        te_state = 1;
        ESP_LOGI(TAG, "TE %lu Hz on GPIO%d (period %.2f ms, high %.2f ms)", (unsigned long)n, PIN_TE,
                 te_period_us / 1000.0f, te_high_us / 1000.0f);
    } else {
        te_state = -1;
        ESP_LOGW(TAG, "TE %lu Hz on GPIO%d: not usable, frames not synced to the panel (tearing possible)",
                 (unsigned long)n, PIN_TE);
    }
}

static uint32_t te_count_ms(int ms)
{
    uint32_t n0 = te_rises;
    vTaskDelay(pdMS_TO_TICKS(ms));
    return te_rises - n0;
}

static void te_init(void)
{
    te_sem = xSemaphoreCreateBinary();
    gpio_config_t te = { .pin_bit_mask = 1ULL << PIN_TE, .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE,
                         .intr_type = GPIO_INTR_ANYEDGE };
    ESP_ERROR_CHECK(gpio_config(&te));
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_TE, te_isr, NULL));
    uint32_t n = te_count_ms(100);
    for (int p = 0; n == 0 && p < 2; p++) {               // no pulse yet: the TE command again
        uint8_t mode = p;
        ESP_LOGI(TAG, "TE: no pulse on GPIO%d in 100 ms, sending 0x35 0x%02x", PIN_TE, mode);
        lcd_cmd(0x35, &mode, 1);
        n = te_count_ms(100);
    }
    te_state = n ? 0 : -1;                                // pulsing: checked over 1 s (te_report)
    te_count0 = te_rises;
    const esp_timer_create_args_t a = { .callback = te_report, .name = "te_check" };
    esp_timer_create(&a, &te_timer);
    esp_timer_start_once(te_timer, 1000000);
    if (!n) ESP_LOGW(TAG, "TE: no pulse on GPIO%d: frames not synced to the panel (tearing possible)", PIN_TE);
}

// "te": TE status and the pushes' numbers; "te lvgl|raw on|off": sync each kind to TE or not; "te log N": the next N
// pushes logged
static void cmd_te(int argc, char **argv)
{
    if (argc == 3 && (!strcmp(argv[1], "lvgl") || !strcmp(argv[1], "raw"))) {
        bool on = !strcmp(argv[2], "on");
        if (!strcmp(argv[1], "lvgl")) te_sync_lvgl = on; else te_sync_raw = on;
        ESP_LOGI("test", "ok te %s %s", argv[1], on ? "on" : "off");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "chase")) {
        te_chase_lvgl = !strcmp(argv[2], "on");
        ESP_LOGI("test", "ok te chase %s", te_chase_lvgl ? "on" : "off");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "log")) {
        te_log_n = atoi(argv[2]);
        ESP_LOGI("test", "ok te log %d", te_log_n);
        return;
    }
    ESP_LOGI("test", "te state=%d rises=%lu period_ms=%.2f high_ms=%.2f level=%d lvgl=%s raw=%s chase=%s",
             te_state, (unsigned long)te_rises, te_period_us / 1000.0f, te_high_us / 1000.0f, gpio_get_level(PIN_TE),
             te_sync_lvgl ? "on" : "off", te_sync_raw ? "on" : "off", te_chase_lvgl ? "on" : "off");
}

void display_register_cmds(void) { testcon_register("te", "te [lvgl|raw|chase on|off] [log N]", cmd_te); }

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
    te_init();

    // Band buffers: internal DMA RAM (L86); the frame: PSRAM (434 KB). One frame is enough: a push copies it into the
    // band buffers and returns when the last band is out, so LVGL may draw into it again at once (a second frame
    // would only add LVGL's copy of each dirty area between the two).
    void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t fb_bytes = FB_STRIDE * DISP_H;
    fb = heap_caps_aligned_alloc(64, fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(b1 && b2 && fb);
    memset(fb, 0, fb_bytes);
    buf1 = b1; buf2 = b2;
    band_done = xSemaphoreCreateCounting(2, 0);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fb, NULL, fb_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, render_evt, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, render_evt, LV_EVENT_RENDER_READY, NULL);

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, &lvgl_th, 1);
    ESP_LOGI(TAG, "Display ready (%dx%d, frame in PSRAM, DIRECT mode)", DISP_W, DISP_H);
}

void display_brightness(uint8_t level)
{
    // Called with the display lock held, from any task. A push returns with its last band done, but wait anyway (any
    // esp_lcd call from outside LVGL must: a call during a transfer can hang for good).
    disp_phase = 7;
    for (int i = 0; i < 100 && __atomic_load_n(&band_inflight, __ATOMIC_ACQUIRE) > 0; i++) vTaskDelay(1);
    disp_phase = 8;
    lcd_cmd(0x51, &level, 1);
    disp_phase = 0;
}
