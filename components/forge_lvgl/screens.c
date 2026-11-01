// Named screens: test console and snapshots (see screens.h)
#include "screens.h"
#include <string.h>
#include "esp_log.h"
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

void screens_testcon(void) { testcon_register("screen", "screen [name]", cmd_screen); }

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
