// The finger, in the browser: the mouse or a touch on the canvas (emu-page.js calls emu_touch). Implements the board's touch
// half (board.h): LVGL's input device and the direct reads of forge_lvgl's slide.c.
#include <emscripten.h>
#include "board.h"
#include "esp_timer.h"

static volatile int down, tx, ty;
// A press LVGL hasn't read yet (a click can be over before it reads). Set by a new press only, not by moves: a drag
// (slide.c) reads the finger itself, and a flag left over from its moves made LVGL read one more press and release
// after it: a tap, which closed weather_amoled's hourly view after every day swipe (October 4). touch_forget() (after each drag) clears it.
static volatile int unseen;
static int64_t last_down;
static touch_read_hook_t hook;
static touch_press_filter_t filter;

EMSCRIPTEN_KEEPALIVE void emu_touch(int d, int x, int y)
{
    if (d && !down) unseen = 1;
    down = d;
    tx = x < 0 ? 0 : x > DISP_W - 1 ? DISP_W - 1 : x;
    ty = y < 0 ? 0 : y > DISP_H - 1 ? DISP_H - 1 : y;
    if (d) last_down = esp_timer_get_time();
}

static void read_cb(lv_indev_t *in, lv_indev_data_t *data)
{
    data->point.x = tx;
    data->point.y = ty;
    data->state = down || unseen ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    unseen = 0;
    // The board's press filter (board.h), as on the device: a refused press is no press until the finger lifts
    static bool was, swallow;
    bool pressed = data->state == LV_INDEV_STATE_PRESSED;
    if (pressed && !was && filter && filter()) swallow = true;
    was = pressed;
    if (swallow) {
        if (!pressed) swallow = false;
        else data->state = LV_INDEV_STATE_RELEASED;
    }
    if (hook) hook(in, data);                            // drags are recognised here (slide.c), before LVGL
}

void touch_register_lvgl(void)
{
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, read_cb);
}
bool touch_fresh(void) { return true; }                 // the mouse never lies
int touch_get(int *x, int *y) { *x = tx; *y = ty; return down; }
void touch_forget(void) { unseen = 0; }
uint32_t touch_idle_ms(void) { return down ? 0 : (uint32_t)((esp_timer_get_time() - last_down) / 1000); }
void touch_set_read_hook(touch_read_hook_t h) { hook = h; }
void touch_set_press_filter(touch_press_filter_t f) { filter = f; }
