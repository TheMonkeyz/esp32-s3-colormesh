// Lock hooks and the simulated finger (see forge_lvgl.h)
#include "forge_lvgl.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "testcon.h"

static const char *TAG = "test";
static const forge_panel_t *panel;
static forge_lock_fn lock_fn;
static forge_unlock_fn unlock_fn;

bool ui_lock(int timeout_ms) { return lock_fn ? lock_fn(timeout_ms) : false; }
void ui_unlock(void) { if (unlock_fn) unlock_fn(); }

static volatile bool inj_on, inj_down;
static volatile int inj_x, inj_y;

void finger_inject(bool down, int x, int y)
{
    int w = lv_display_get_horizontal_resolution(NULL), h = lv_display_get_vertical_resolution(NULL);
    inj_x = x < 0 ? 0 : x >= w ? w - 1 : x;
    inj_y = y < 0 ? 0 : y >= h ? h - 1 : y;
    inj_down = down;
    inj_on = true;
}

void finger_inject_end(void) { inj_down = false; inj_on = false; }

bool finger_injected(int *x, int *y, bool *down)
{
    if (!inj_on) return false;
    *x = inj_x; *y = inj_y; *down = inj_down;
    return true;
}

// Finger moves in steps of ~16 ms (LVGL reads the touch every ~15-30 ms, so every read sees a new point)
static void finger_path(int x0, int y0, int x1, int y1, int ms)
{
    int steps = ms / 16 < 2 ? 2 : ms / 16;
    for (int i = 0; i <= steps; i++) {
        finger_inject(true, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

static void finger_up(int x, int y)
{
    finger_inject(false, x, y);
    vTaskDelay(pdMS_TO_TICKS(100));          // LVGL must read the release before the real controller takes over
    finger_inject_end();
}

static void press(int x, int y, int ms)
{
    finger_inject(true, x, y);
    vTaskDelay(pdMS_TO_TICKS(ms));
    finger_up(x, y);
}

static int ms_arg(const char *s)                          // 0..10000 ms
{
    int v = atoi(s);
    return v < 0 ? 0 : v > 10000 ? 10000 : v;
}

// A real tap is ~80-150 ms; 60 ms fitted inside one full redraw and LVGL never saw it
static void cmd_tap(int argc, char **argv)
{
    if (argc != 3) { ESP_LOGW(TAG, "error tap X Y"); return; }
    press(atoi(argv[1]), atoi(argv[2]), 120);
    ESP_LOGI(TAG, "ok tap");
}

static void cmd_press(int argc, char **argv)                // long-press: press X Y [ms, default 1200]
{
    if (argc < 3) { ESP_LOGW(TAG, "error press X Y [ms]"); return; }
    press(atoi(argv[1]), atoi(argv[2]), argc > 3 ? ms_arg(argv[3]) : 1200);
    ESP_LOGI(TAG, "ok press");
}

// swipe DIR [DIR...]: several swipes with 150 ms of "up" between them: a real lift (slide.c bridges "ups" under 60 ms,
// the chip's false ones, and 70 ms read at 10 ms intervals was sometimes bridged: one drag left then right), yet the
// next press lands while the previous move's release animation still runs (~280 ms), as a quick finger does (L181)
static void cmd_swipe(int argc, char **argv)
{
    int cx = lv_display_get_horizontal_resolution(NULL) / 2, cy = lv_display_get_vertical_resolution(NULL) / 2;
    int d = (cx < cy ? cx : cy) * 2 / 3;
    if (argc < 2 || argc > 5) { ESP_LOGW(TAG, "error swipe: left, right, up or down (up to 4)"); return; }
    for (int i = 1; i < argc; i++) {
        const char *dir = argv[i];
        if (strcmp(dir, "left") && strcmp(dir, "right") && strcmp(dir, "up") && strcmp(dir, "down")) {
            ESP_LOGW(TAG, "error swipe: left, right, up or down");
            return;
        }
    }
    int x1 = cx, y1 = cy;
    for (int i = 1; i < argc; i++) {
        const char *dir = argv[i];
        int x0 = cx, y0 = cy;
        x1 = cx; y1 = cy;
        if (!strcmp(dir, "left"))       { x0 = cx + d; x1 = cx - d; }
        else if (!strcmp(dir, "right")) { x0 = cx - d; x1 = cx + d; }
        else if (!strcmp(dir, "up"))    { y0 = cy + d; y1 = cy - d; }
        else                            { y0 = cy - d; y1 = cy + d; }
        finger_path(x0, y0, x1, y1, 200);
        if (i < argc - 1) { finger_inject(false, x1, y1); vTaskDelay(pdMS_TO_TICKS(150)); }
    }
    finger_up(x1, y1);
    ESP_LOGI(TAG, "ok swipe %s%s", argv[1], argc > 2 ? " ..." : "");
}

static void cmd_drag(int argc, char **argv)                 // drag X1 Y1 X2 Y2 [ms]
{
    if (argc < 5) { ESP_LOGW(TAG, "error drag X1 Y1 X2 Y2 [ms]"); return; }
    int x1 = atoi(argv[3]), y1 = atoi(argv[4]);
    finger_path(atoi(argv[1]), atoi(argv[2]), x1, y1, argc > 5 ? ms_arg(argv[5]) : 400);
    finger_up(x1, y1);
    ESP_LOGI(TAG, "ok drag");
}

void screens_testcon(void);                                 // screens.c
void slide_init(void);                                      // slide.c

const forge_panel_t *forge_lvgl_panel(void) { return panel; }

void forge_lvgl_set_panel(const forge_panel_t *p)
{
    panel = p;
    ui_lock(-1);
    slide_init();
    ui_unlock();
}

void forge_lvgl_init(forge_lock_fn lock, forge_unlock_fn unlock)
{
    lock_fn = lock;
    unlock_fn = unlock;
    testcon_register("tap", "tap X Y", cmd_tap);
    testcon_register("press", "press X Y [ms]", cmd_press);
    testcon_register("swipe", "swipe left|right|up|down [more...]", cmd_swipe);
    testcon_register("drag", "drag X1 Y1 X2 Y2 [ms]", cmd_drag);
    screens_testcon();
}
