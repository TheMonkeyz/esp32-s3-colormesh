// The panel, in the browser: a DISP_W x DISP_H RGB565 framebuffer the page copies to its canvas (emu-page.js, on every
// animation frame when emu_fb_dirty() says so). Implements the board's display half (board.h, the API every board
// exposes): board_init() as the board's (LVGL, the display, the touch, forge_lvgl's hooks), LVGL's flush and the raw
// frames of forge_lvgl's slide.c. emu_brightness() is display_brightness()'s level: the page dims the canvas with it.
#include <string.h>
#include <emscripten.h>
#include "board.h"
#include "forge_lvgl.h"

static uint16_t fb[DISP_W * DISP_H];       // RGB565 as LVGL draws it
static int dirty = 1;
static display_flush_hook_t flush_hook;
static uint8_t level = 255;

EMSCRIPTEN_KEEPALIVE uint16_t *emu_fb(void) { return fb; }
EMSCRIPTEN_KEEPALIVE int emu_fb_dirty(void) { int d = dirty; dirty = 0; return d; }
EMSCRIPTEN_KEEPALIVE int emu_brightness(void) { return level; }

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    if (flush_hook) flush_hook(a, px);
    int w = a->x2 - a->x1 + 1;
    const uint16_t *src = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++) {
        if (y < 0 || y >= DISP_H) { src += w; continue; }
        memcpy(&fb[y * DISP_W + a->x1], src, w * 2);
        src += w;
    }
    dirty = 1;
    lv_display_flush_ready(disp);
}

static void display_init(void)
{
    static uint16_t b1[DISP_W * 32], b2[DISP_W * 32];      // 32-row bands, as the display's
    lv_display_t *d = lv_display_create(DISP_W, DISP_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, b1, b2, sizeof(b1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
}

bool display_lock(int timeout_ms) { (void)timeout_ms; return true; }   // one thread
void display_unlock(void) {}
void display_brightness(uint8_t l) { level = l; }
static bool asleep;                                       // the panel sleep (board.h): kept as a flag
void display_sleep(bool off) { asleep = off; }
bool display_asleep(void) { return asleep; }
bool display_cpu_full_speed(void) { return !asleep; }
void display_get_stats(display_stats_t *out, bool reset) { (void)reset; memset(out, 0, sizeof(*out)); }
void display_set_flush_hook(display_flush_hook_t hook) { flush_hook = hook; }

static inline uint16_t unswap(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }

// slide.c's frames are in the panel's byte order (swapped): back to LVGL's, row band by row band
void display_raw_frame(display_fill_cb_t fill, void *user)
{
    static uint16_t band[DISP_W * 32];
    for (int y0 = 0; y0 < DISP_H; y0 += 32) {
        int n = y0 + 32 > DISP_H ? DISP_H - y0 : 32;
        fill(y0, n, band, user);
        for (int i = 0; i < DISP_W * n; i++) fb[y0 * DISP_W + i] = unswap(band[i]);
    }
    dirty = 1;
    emscripten_sleep(0);                                   // let the page show it (the browser paints between frames)
}

// A part of the screen (slide.c's list scrolls): the same, for columns x0..x0 + w - 1 of rows y0..y1
void display_raw_area(int x0, int y0, int x1, int y1, bool bottom_up, display_area_fill_cb_t fill, void *user)
{
    static uint16_t band[DISP_W * 32];
    (void)bottom_up;                                       // the frame buffer takes the rows in any order
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DISP_W - 1) x1 = DISP_W - 1;
    if (y1 > DISP_H - 1) y1 = DISP_H - 1;
    int w = x1 - x0 + 1;
    for (int y = y0; y <= y1; y += 32) {
        int n = y + 32 > y1 + 1 ? y1 + 1 - y : 32;
        fill(x0, w, y, n, band, user);
        for (int r = 0; r < n; r++)
            for (int c = 0; c < w; c++) fb[(y + r) * DISP_W + x0 + c] = unswap(band[r * w + c]);
    }
    dirty = 1;
    emscripten_sleep(0);
}

static uint32_t tick(void) { return (uint32_t)emscripten_get_now(); }

void touch_register_lvgl(void);                            // emu_touch.c
void touch_set_read_hook(touch_read_hook_t hook);

// Direct panel and touch access for forge_lvgl's moves drawn as pictures (slide.h), as the board's
static const forge_panel_t panel = {
    .raw_frame = display_raw_frame,
    .set_flush_hook = display_set_flush_hook,
    .set_read_hook = touch_set_read_hook,
    .touch_get = touch_get,
    .touch_fresh = touch_fresh,
    .touch_forget = touch_forget,
    .raw_area = display_raw_area,
};

// main.c calls it after the texts, as on the display
void board_init(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    display_init();
    touch_register_lvgl();
    forge_lvgl_init(display_lock, display_unlock);
    forge_lvgl_set_panel(&panel);
}

i2c_master_bus_handle_t board_i2c_bus(void) { return NULL; }
