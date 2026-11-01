// Named screens: test console and snapshots (see screens.h)
#include "screens.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "testcon.h"
#include "forge_lvgl.h"

static const char *TAG = "test";
static const screen_def_t *defs;
static int ndefs;

void screens_register(const screen_def_t *d, int n) { defs = d; ndefs = n; }

static const screen_def_t *find(const char *name)
{
    for (int i = 0; i < ndefs; i++) if (!strcmp(defs[i].name, name)) return &defs[i];
    return NULL;
}

// Shown = the active screen, or a visible object on it (an overlay). The last match wins: register overlays after
// the screen they cover.
static const char *current_locked(void)
{
    const char *name = "other";
    lv_obj_t *act = lv_screen_active();
    for (int i = 0; i < ndefs; i++) {
        if (defs[i].shown) { if (defs[i].shown()) name = defs[i].name; continue; }
        lv_obj_t *o = defs[i].get();
        if (o == act || (o && o != act && lv_obj_get_screen(o) == act && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)))
            name = defs[i].name;
    }
    return name;
}

const char *screens_current(void)
{
    if (!ui_lock(2000)) return "busy";
    const char *n = current_locked();
    ui_unlock();
    return n;
}

static void cmd_screen(int argc, char **argv)
{
    if (!ui_lock(2000)) { ESP_LOGW(TAG, "error screen: display busy for 2 s (send 'where')"); return; }
    if (argc == 1) {
        const char *n = current_locked();
        ui_unlock();
        ESP_LOGI(TAG, "screen %s", n);
        return;
    }
    const screen_def_t *d = find(argv[1]);
    if (d) { if (d->show) d->show(); else lv_screen_load(d->get()); }
    ui_unlock();
    if (d) ESP_LOGI(TAG, "ok screen %s", argv[1]);
    else ESP_LOGW(TAG, "error screen: no screen '%s'", argv[1]);
}

/* "shot [SCREEN [DIV]]": a screen (a registered name, "current" by default) rendered off-display and sent over the USB
 * console as base64 RGB565, every DIV-th pixel of every DIV-th row (DIV 1..4, default 2): "SHOT name w h", one "S:" line
 * per row, "SHOTEND". tools/devloop/shot.py turns a log into PNGs. For a session that can't reach the board's HTTPS
 * /api/snapshot (the cloud's, a board behind a co-processor not yet reachable), and for screens before Wi-Fi. Straight
 * to the USB driver (testcon's): a UART console would take ~40 s. From MeteoBus's P4 port (L208). "panel" (the frame
 * buffer as shown) exists on MeteoBus's P4 only: this board renders in bands. */
static void usb_line(const char *s, size_t n)
{
    for (size_t o = 0; o < n;) {
        size_t k = n - o > 128 ? 128 : n - o;             // the driver's ring buffer refuses bigger writes
        int w = usb_serial_jtag_write_bytes(s + o, k, pdMS_TO_TICKS(500));
        if (w <= 0) return;                               // no host reading: drop the rest
        o += w;
    }
}

static char shot_name[24];
static int shot_div;

static void shot_task(void *arg)                          // its own task: rendering a screen needs a big stack
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    web_image_t img = {0};
    if (!screens_snapshot(shot_name, &img)) { ESP_LOGW(TAG, "error shot %s: no such screen", shot_name); vTaskDelete(NULL); return; }
    int div = shot_div, w = img.w / div, h = img.h / div;
    uint16_t *row = heap_caps_malloc(w * 2, MALLOC_CAP_SPIRAM);
    char *line = heap_caps_malloc(w * 2 * 4 / 3 + 16, MALLOC_CAP_SPIRAM);
    if (row && line) {
        char hd[64];
        int n = snprintf(hd, sizeof(hd), "\nSHOT %s %d %d\n", shot_name, w, h);
        usb_line(hd, n);
        for (int y = 0; y < h; y++) {
            const uint16_t *src = (const uint16_t *)(img.data + (size_t)y * div * img.stride);
            for (int x = 0; x < w; x++) row[x] = src[x * div];
            const uint8_t *b = (const uint8_t *)row;
            int len = w * 2, o = 0;
            line[o++] = 'S'; line[o++] = ':';
            for (int i = 0; i < len; i += 3) {
                uint32_t v = b[i] << 16 | (i + 1 < len ? b[i + 1] << 8 : 0) | (i + 2 < len ? b[i + 2] : 0);
                line[o++] = b64[v >> 18 & 63];
                line[o++] = b64[v >> 12 & 63];
                line[o++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
                line[o++] = i + 2 < len ? b64[v & 63] : '=';
            }
            line[o++] = '\n';
            usb_line(line, o);
        }
        usb_line("SHOTEND\n", 8);
    }
    free(row);
    free(line);
    screens_snapshot_free(&img);
    // The rows filled the USB driver's ring: let it drain before the log line, which the console dropped otherwise
    // (the harness waited 30 s for "ok shot" after a complete picture)
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "ok shot %s %dx%d", shot_name, w, h);
    vTaskDelete(NULL);
}

static void cmd_shot(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "panel")) {
        ESP_LOGW(TAG, "error shot panel: this board renders in bands (no frame buffer); name a screen");
        return;
    }
    strlcpy(shot_name, argc > 1 ? argv[1] : "current", sizeof(shot_name));
    shot_div = argc > 2 ? atoi(argv[2]) : 2;
    if (shot_div < 1 || shot_div > 4) shot_div = 2;
    if (xTaskCreatePinnedToCore(shot_task, "shot", 12288, NULL, 2, NULL, 0) != pdPASS)
        ESP_LOGW(TAG, "error shot %s: no task", shot_name);
}

void screens_testcon(void)
{
    testcon_register("screen", "screen [name]", cmd_screen);
    testcon_register("shot", "shot [SCREEN [DIV]]", cmd_shot);
}

// lv_snapshot_take renders into LVGL's heap (PSRAM): works for screens not shown, without touching the panel
bool screens_snapshot(const char *name, web_image_t *out)
{
    bool current = !strcmp(name, "current");
    const screen_def_t *d = current ? NULL : find(name);
    if (!current && !d) return false;
    ui_lock(-1);
    lv_obj_t *s = d ? d->get() : lv_screen_active();
    if (d && d->prepare) d->prepare();
    lv_obj_update_layout(s);
    lv_draw_buf_t *db = lv_snapshot_take(s, LV_COLOR_FORMAT_RGB565);
    ui_unlock();
    if (!db) return false;
    *out = (web_image_t){ .data = db->data, .w = db->header.w, .h = db->header.h, .stride = db->header.stride,
                          .priv = db };
    return true;
}

void screens_snapshot_free(web_image_t *img)
{
    if (!img->priv) return;
    ui_lock(-1);
    lv_draw_buf_destroy(img->priv);
    ui_unlock();
    img->priv = NULL;
}
