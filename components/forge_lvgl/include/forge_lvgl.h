#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// LVGL helpers shared by every espforge app. The board owns LVGL (display driver, task, lock); it tells forge_lvgl
// how to take the lock with forge_lvgl_init(). Everything here that touches LVGL takes that lock, and gives up after
// 2 s when called from the test console, so "where" can still answer.

typedef bool (*forge_lock_fn)(int timeout_ms);   // -1 = forever
typedef void (*forge_unlock_fn)(void);
// Called by the board once LVGL and its input device exist. Registers the test console's touch and screen commands.
void forge_lvgl_init(forge_lock_fn lock, forge_unlock_fn unlock);
bool ui_lock(int timeout_ms);
void ui_unlock(void);

/* ---- simulated finger (test console) ----
 * tap/press/swipe/drag replace the touch controller's report while active, so long-press, gestures and scrolling go
 * through exactly the same code as a real finger. The board's touch read callback asks finger_injected() first. */
void finger_inject(bool down, int x, int y);   // screen pixels
void finger_inject_end(void);                  // back to the real controller
bool finger_injected(int *x, int *y, bool *down);

/* ---- the panel, for moves drawn outside LVGL (slide.h) ----
 * The board gives forge_lvgl direct access to its panel and touch chip (forge_lvgl can't include board.h: the board
 * depends on it). Without it, slide.c falls back to LVGL's own (slower) animations. */
typedef struct {
    void (*raw_frame)(void (*fill)(int y0, int n, void *dst, void *user), void *user);   // see board.h
    void (*set_flush_hook)(void (*hook)(const lv_area_t *a, const uint8_t *px));
    void (*set_read_hook)(void (*hook)(lv_indev_t *in, lv_indev_data_t *data));
    int (*touch_get)(int *x, int *y);      // 1 down, 0 up, -1 bus error; the chip read at most every 10 ms
    bool (*touch_fresh)(void);
    void (*touch_forget)(void);
    // A part of the screen without LVGL (slide.c's list scrolls; board.h display_raw_area). NULL: lists stay LVGL's.
    void (*raw_area)(int x0, int y0, int x1, int y1, bool bottom_up,
                     void (*fill)(int x0, int w, int y0, int n, void *dst, void *user), void *user);
} forge_panel_t;
void forge_lvgl_set_panel(const forge_panel_t *panel);   // board_init, after forge_lvgl_init

uint32_t lvgl_mem_fallbacks(void);             // LVGL blocks put in internal RAM because PSRAM was full
