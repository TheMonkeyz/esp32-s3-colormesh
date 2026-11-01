#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "driver/i2c_master.h"

// Board support: Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 466x466 round AMOLED over QSPI, CST9217 touch and
// QMI8658 motion sensor on I2C). Every board directory (boards/<name>/board) exposes this same API; the root
// CMakeLists.txt picks one with EXTRA_COMPONENT_DIRS (docs/NEW-PROJECT.md, "A new board").

#define BOARD_NAME  "Waveshare ESP32-S3-Touch-AMOLED-1.75"
#define DISP_W      466
#define DISP_H      466
#define BOARD_ROUND 1           // round panel: keep content inside the circle (snapshots tint the corners red)

// Display, LVGL (its task on core 1), touch and the forge_lvgl hooks, the test console's "fps" and "where"
// breadcrumbs, and the diag "display:" line. Call after nvs/net init is not required; call before building screens.
void board_init(void);

// The LVGL lock (recursive). Every LVGL call from outside the LVGL task holds it. Never hold it for long: a task
// that blocks LVGL for more than ~50 ms loses quick flicks.
bool display_lock(int timeout_ms);   // -1 = forever
void display_unlock(void);
void display_brightness(uint8_t level);   // 0..255, with the lock held, from any task
// The panel asleep while the screen is off (forge_presence's screen_off hook, through the app): DISPOFF and SLPIN from
// any task with the lock held; waking sends SLPOUT, and the LVGL task finishes 120 ms later with a whole frame and
// DISPON (L81: a whole refresh from core 0 would race the transfer-done interrupt on core 1). While the panel is awake
// a "screen" power-management lock keeps the CPU at 240 MHz even idle; asleep, an idle core may drop to 80 MHz
// (CONFIG_PM_ENABLE; board_init configures 80..240 MHz, no light sleep). The console's "power" shows both.
void display_sleep(bool off);
bool display_asleep(void);
bool display_cpu_full_speed(void);        // the "screen" lock is held

// Frame statistics since the last reset (diag line every period; "fps" in the test console has its own counters).
// render_us: LVGL's drawing alone (the push and its wait for TE are counted apart)
typedef struct {
    uint32_t frames, render_us, render_max_us;     // rendered frames and their render time
    uint32_t anim_frames, anim_us, anim_gap_max_us; // back-to-back frames (animations): interval sum / worst gap
    uint32_t anim_gap_max_at_ms;                   // ...when it ended (ms since the reset; test console only)
    char anim_gap_max_kind[12];                    // ...between which frames: "lvgl>move", "move>move", ...
    uint64_t pixels;                               // pixels sent to the panel
    // Pushes to the panel (LVGL frames, raw ones and parts of the screen), each started on the panel's TE pulse
    // (display.c; a part sent bottom up and pushes while the panel sleeps don't wait for it)
    uint32_t pushes, push_us, push_max_us;         // their time on the bus (from the TE edge to the last band done)
    uint32_t te_wait_us, te_wait_max_us;           // their wait for the TE edge (at most 25 ms)
    uint32_t te_lag_max_us;                        // longest time from a TE edge to the push starting
    uint32_t te_timeouts;                          // pushes that started after 25 ms without a TE edge
    uint32_t te_chased;                            // pushes started off the edge, behind or ahead of the scan
    uint32_t lvgl_wait_max_us;                     // longest wait of the LVGL task for the lock
    uint32_t hold_max_us;                          // longest lock hold by another task...
    char hold_task[16];                            // ...and which task
} display_stats_t;
void display_get_stats(display_stats_t *out, bool reset);

// LVGL draws into one whole frame in PSRAM (DIRECT mode); each finished frame goes to the panel on its TE pulse (no
// tearing). Console: "te [lvgl|raw|chase on|off] [log N]"; the boot line "display: TE <n> Hz on GPIO13 (...)".
// A frame drawn without LVGL (forge_lvgl's slide.c): fill() writes rows y0..y0+n-1, full width, RGB565
// *byte-swapped* (panel order), into dst; each band is sent while the next one is filled, the first on TE. Display
// lock held; afterwards LVGL must redraw before it flushes again (lv_obj_invalidate): LVGL's frame keeps its own.
typedef void (*display_fill_cb_t)(int y0, int n, void *dst, void *user);
void display_raw_frame(display_fill_cb_t fill, void *user);
// A part of the screen (x0..x1, y0..y1) without LVGL (slide.c's list scrolls): fill() writes rows y0..y0+n-1 of
// columns x0..x0+w-1 (RGB565 byte-swapped) into dst; bottom_up sends the bands from the bottom (rows moved down in
// place need the lower rows first). Even start / odd end as the panel wants. Display lock held. Top down: started on
// TE; bottom up: at once (it runs against the panel's scan). Also copied into LVGL's frame, which stays the panel's.
typedef void (*display_area_fill_cb_t)(int x0, int w, int y0, int n, void *dst, void *user);
void display_raw_area(int x0, int y0, int x1, int y1, bool bottom_up, display_area_fill_cb_t fill, void *user);

// Called with every area LVGL sends to the panel, before the byte swap (RGB565 as LVGL draws it), in the LVGL task:
// whole-width rows of LVGL's frame (the whole frame at a wake). For a picture of the screen kept in step with the
// panel (drags drawn outside LVGL).
typedef void (*display_flush_hook_t)(const lv_area_t *a, const uint8_t *px);
void display_set_flush_hook(display_flush_hook_t hook);

i2c_master_bus_handle_t board_i2c_bus(void);   // the touch bus, shared with the motion sensor (imu.h) and audio
uint32_t touch_idle_ms(void);                  // ms since a finger was last down (any task)
// The finger now, read directly, for moves drawn outside LVGL (display lock held, LVGL not reading meanwhile):
// 1 pressed at x,y, 0 up, -1 bus error. The chip is read at most every 10 ms: polled faster it answers "not in
// contact" for long stretches with the finger on it. touch_fresh(): the last touch_get() read the chip.
int touch_get(int *x, int *y);
bool touch_fresh(void);
void touch_forget(void);                       // the touch LVGL last saw was handled by a drag: start afresh
// Called with each touch read LVGL makes, before LVGL handles it (gesture recognisers, waking a dark screen)
typedef void (*touch_read_hook_t)(lv_indev_t *indev, lv_indev_data_t *data);
void touch_set_read_hook(touch_read_hook_t hook);
// Asked when a finger comes down, before LVGL and the read hook see it: true = ignore this whole press until the
// finger lifts (LVGL and the read hook see no finger; touch_get() and touch_idle_ms() still do). For "a touch on a
// dark screen only wakes it" (from esp32-s3-rtcquebec v0.3.0).
typedef bool (*touch_press_filter_t)(void);
void touch_set_press_filter(touch_press_filter_t filter);
