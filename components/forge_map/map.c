// The map task and its kept pictures (forge_map.h): fmap_set_center() picks a slot (map_slots.c) and wakes the task,
// the task draws the current slot's tiles (map_draw.c) and reports each one. One mutex guards the slots' bookkeeping;
// the pixels themselves are written by the task without it (an app reading px meanwhile sees a tile half drawn, then
// the next update).
#include "map_priv.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "svc.h"
#include "net.h"

static const char *TAG = "fmap";
// The task's stack, internal RAM: the TLS handshake of esp_http_client runs in it (rtcquebec's map task: 6 KB) plus
// the tile URL (256 bytes); the body (PSRAM) and png_rows' ~50 KB (PSRAM) are not on it
#define TASK_STACK (7 * 1024)

typedef struct {
    uint16_t *px;                      // allocated on the slot's first use, kept (reused for the next view)
    fmap_state_t state;
    int tiles, done;
    uint32_t path_stamp;               // the path drawn into it (m.path_stamp when current)
} slot_t;

#define UNDO_MAX 32768                 // pixels under the path kept for one picture (6 bytes each: 192 KB of PSRAM)

// All of it in PSRAM (L185: plain statics are internal RAM, the scarce kind, in every app that links the component).
// .ext_ram.bss is zeroed at start but takes no initialisers: slot numbers are kept + 1 (0 = none), as in fmap_slots_t.
EXT_RAM_BSS_ATTR static struct {
    fmap_opts_t o;
    fmap_slots_t t;                    // keys, LRU stamps, the current / previous / busy slot
    slot_t slot[FMAP_SLOTS_MAX];
    int svc;
    SemaphoreHandle_t mu;
    TaskHandle_t task;
    // The path (fmap_set_path): the places, how it is drawn, a stamp per change; the pixels under it in one picture
    fmap_place_t path[FMAP_PATH_MAX];
    int path_n, path_w;
    uint16_t path_c;
    uint8_t path_opa;
    uint32_t path_stamp;
    fmap_undo_t *undo;                 // allocated with the first path
    int undo_n, undo_slot1;            // undo_slot1: the slot whose pixels these are, + 1 (0: none)
    bool undo_full;
    uint8_t *mask;                     // w x h bits for fmap_path_draw
    // The flash cache: the home place (fmap_set_home) and the partition's I/O table
    bool home_set;
    double home_lat, home_lon;
    const fmap_cache_io_t *cache;
} m;

__attribute__((weak)) const fmap_cache_io_t *fmap_cache_flash_io(void (*pause)(void *ctx)) { (void)pause; return NULL; }

static void cache_pause(void *ctx)     // between sectors: the idle task runs; longer while a finger is on the screen
{
    (void)ctx;
    for (int i = 0; i < 200 && m.o.busy && m.o.busy(); i++) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(1);
}

static bool is_home(int zoom, double ox, double oy)
{
    if (!m.home_set) return false;
    double hx, hy;
    geo_origin(m.home_lat, m.home_lon, zoom, m.o.w, m.o.h, &hx, &hy);
    return hx == ox && hy == oy;
}

// The path into slot s's picture (lock held): the pixels under the old one back first when they are this slot's; a
// slot whose old path can't be undone is given up (downloaded again when shown next)
static void path_apply(int s)
{
    slot_t *sl = &m.slot[s];
    if (!sl->px) return;
    if (m.undo_slot1 == s + 1) {
        if (m.undo_full) { m.t.key[s].valid = false; m.undo_slot1 = 0; return; }
        fmap_path_undo(sl->px, m.undo, m.undo_n);
    } else if (m.undo_slot1 && m.slot[m.undo_slot1 - 1].path_stamp && m.undo_slot1 - 1 != s) {
        m.t.key[m.undo_slot1 - 1].valid = false;          // its path stays drawn: not kept
    }
    m.undo_slot1 = 0;
    m.undo_n = 0;
    sl->path_stamp = m.path_stamp;
    if (!m.path_n) return;
    if (!m.mask) m.mask = heap_caps_malloc(((size_t)m.o.w * m.o.h + 7) / 8, MALLOC_CAP_SPIRAM);
    if (!m.undo) m.undo = heap_caps_malloc(UNDO_MAX * sizeof(fmap_undo_t), MALLOC_CAP_SPIRAM);
    if (!m.mask) return;
    m.undo_n = fmap_path_draw(sl->px, m.o.w, m.o.h, m.t.key[s].zoom, m.t.key[s].ox, m.t.key[s].oy, m.path, m.path_n,
                              m.path_c, m.path_w, m.path_opa, m.mask, m.undo, m.undo ? UNDO_MAX : 0, &m.undo_full);
    m.undo_slot1 = s + 1;
}

static void lock(void) { xSemaphoreTake(m.mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(m.mu); }
static void tell(void) { if (m.o.updated) m.o.updated(m.o.user); }

// fmap_draw's hooks, for slot s (in user): stop once set_center has moved on to another slot; count the tiles drawn
static bool moved_on(void *user)
{
    lock();
    bool moved = m.t.cur1 != (int)(intptr_t)user + 1;
    unlock();
    return moved;
}

static void progress(int ok, int total, void *user)
{
    lock();
    m.slot[(int)(intptr_t)user].done = ok;
    unlock();
    tell();
}

static void map_task(void *arg)
{
    fmap_body_t b = { 0 };
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {                                        // the current slot while it waits for tiles
            lock();
            int s = m.t.cur1 - 1;
            bool go = s >= 0 && m.t.key[s].valid && m.slot[s].state == FMAP_LOADING;
            fmap_slot_key_t k = go ? m.t.key[s] : (fmap_slot_key_t){ 0 };
            uint16_t *px = go ? m.slot[s].px : NULL;
            m.t.busy1 = go ? s + 1 : 0;
            unlock();
            if (!go) break;
            if (!net_is_connected()) {                    // not a failure: the picture waits for Wi-Fi
                lock();
                m.t.busy1 = 0;
                unlock();
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            // The flash cache first: the home place's picture at this zoom, kept from an earlier boot
            size_t coff = 0;
            bool home = is_home(k.zoom, k.ox, k.oy);
            bool cached = m.cache && home && fmap_cache_slot(m.cache, m.o.w, m.o.h, k.zoom, m.o.zoom_min, &coff);
            if (cached && fmap_cache_load(m.cache, coff, k.zoom, (int32_t)k.ox, (int32_t)k.oy, px, m.o.w, m.o.h)) {
                ESP_LOGI(TAG, "zoom %d at %.0f,%.0f: from the flash cache", k.zoom, k.ox, k.oy);
                lock();
                m.slot[s].state = FMAP_READY;
                m.slot[s].done = m.slot[s].tiles;
                path_apply(s);
                m.t.busy1 = 0;
                unlock();
                tell();
                continue;
            }
            if (!net_dl_begin("map", 30000)) {              // one TLS download at a time (net.h): try again later
                lock();
                m.t.busy1 = 0;
                unlock();
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            int ok = 0, total = 0;
            bool all = fmap_draw(&m.o, m.svc, k.zoom, k.ox, k.oy, px, m.o.w, m.o.h, &b, moved_on, (void *)(intptr_t)s,
                                 progress, (void *)(intptr_t)s, &ok, &total);
            net_dl_end("map");
            // Complete and the home's: into the flash cache (the tiles alone, before the path), sector by sector
            if (all && cached && fmap_cache_save(m.cache, coff, k.zoom, (int32_t)k.ox, (int32_t)k.oy, px, m.o.w, m.o.h))
                ESP_LOGI(TAG, "zoom %d at %.0f,%.0f: kept in the flash cache", k.zoom, k.ox, k.oy);
            lock();
            // Cancelled or short of tiles: FAILED, kept with what it has; set_center tries it again when it comes back
            m.slot[s].state = all ? FMAP_READY : FMAP_FAILED;
            m.slot[s].done = ok;
            path_apply(s);
            m.t.busy1 = 0;
            unlock();
            tell();
        }
        free(b.buf);                                      // up to 256 KB of PSRAM back between maps
        b = (fmap_body_t){ 0 };
    }
}

bool fmap_create(const fmap_opts_t *opts)
{
    if (m.mu) { ESP_LOGE(TAG, "fmap_create: already created"); return false; }
    if (!opts || opts->w <= 0 || opts->h <= 0 || !opts->tile_url || opts->zoom_min < 0 ||
        opts->zoom_max > GEO_ZOOM_MAX || opts->zoom_min > opts->zoom_max) {
        ESP_LOGE(TAG, "fmap_create: unusable options");
        return false;
    }
    m.o = *opts;
    m.t.n = opts->psram_slots < 1 ? 1 : opts->psram_slots > FMAP_SLOTS_MAX ? FMAP_SLOTS_MAX : opts->psram_slots;
    m.svc = -1;
    if (opts->svc_name && (m.svc = svc_find(opts->svc_name)) < 0) m.svc = svc_add(opts->svc_name, opts->svc_api, NULL);
    if (!(m.mu = xSemaphoreCreateMutex())) return false;
    if (opts->flash_cache) m.cache = fmap_cache_flash_io(cache_pause);
    // Core 0, below LVGL's task: the decode yields every 64 rows (map_draw.c)
    if (xTaskCreatePinnedToCore(map_task, "fmap", TASK_STACK, NULL, 3, &m.task, 0) != pdPASS) {
        ESP_LOGE(TAG, "fmap_create: no memory for the task");
        m.task = NULL;
        return false;
    }
    return true;
}

static void fill(fmap_view_t *out, int s)
{
    const slot_t *sl = &m.slot[s];
    *out = (fmap_view_t){ .px = sl->px, .w = m.o.w, .h = m.o.h, .zoom = m.t.key[s].zoom, .ox = m.t.key[s].ox,
                          .oy = m.t.key[s].oy, .state = sl->state, .tiles = sl->tiles, .done = sl->done };
}

void fmap_set_center(double lat, double lon, int zoom, fmap_view_t *out)
{
    if (!m.mu) { *out = (fmap_view_t){ .state = FMAP_FAILED }; return; }
    zoom = zoom < m.o.zoom_min ? m.o.zoom_min : zoom > m.o.zoom_max ? m.o.zoom_max : zoom;
    double ox, oy;
    geo_origin(lat, lon, zoom, m.o.w, m.o.h, &ox, &oy);
    lock();
    bool hit;
    int s = fmap_slots_take(&m.t, zoom, ox, oy, &hit);
    if (s < 0) {                                          // only with 1 slot while the task is still drawing into it
        unlock();
        ESP_LOGW(TAG, "zoom %d at %.0f,%.0f: every picture in use", zoom, ox, oy);
        *out = (fmap_view_t){ .w = m.o.w, .h = m.o.h, .zoom = zoom, .ox = ox, .oy = oy, .state = FMAP_FAILED };
        return;
    }
    slot_t *sl = &m.slot[s];
    if (hit && sl->state == FMAP_FAILED) sl->state = FMAP_LOADING;   // a failed or cancelled one: tried again
    else if (hit && sl->state == FMAP_READY) {
        ESP_LOGI(TAG, "zoom %d at %.0f,%.0f: kept", zoom, ox, oy);
        if (sl->path_stamp != m.path_stamp) path_apply(s);          // the path changed since it was drawn
    }
    if (!hit) {                                           // a new picture in that slot (its key is set)
        if (!sl->px) sl->px = heap_caps_malloc((size_t)m.o.w * m.o.h * 2, MALLOC_CAP_SPIRAM);
        geo_cover_t c;
        geo_cover(ox, oy, m.o.w, m.o.h, zoom, &c);
        sl->tiles = c.count;
        sl->done = 0;
        sl->state = sl->px ? FMAP_LOADING : FMAP_FAILED;
        m.t.key[s].valid = sl->px != NULL;                // (no picture: asked again next time)
        if (sl->px) for (size_t i = 0; i < (size_t)m.o.w * m.o.h; i++) sl->px[i] = m.o.empty565;
        else ESP_LOGE(TAG, "no memory for a map picture (%d KB)", m.o.w * m.o.h * 2 / 1024);
    }
    fill(out, s);
    bool start = sl->state == FMAP_LOADING;
    unlock();
    if (start && m.task) xTaskNotifyGive(m.task);
}

void fmap_status(fmap_view_t *out)
{
    if (!m.mu) { *out = (fmap_view_t){ .state = FMAP_FAILED }; return; }
    lock();
    if (m.t.cur1) fill(out, m.t.cur1 - 1);
    else *out = (fmap_view_t){ .state = FMAP_FAILED };
    unlock();
}

const char *fmap_attribution(void) { return m.o.attribution ? m.o.attribution : ""; }

void fmap_set_home(double lat, double lon)
{
    if (!m.mu) return;
    lock();
    m.home_set = true;
    m.home_lat = lat;
    m.home_lon = lon;
    unlock();
}

void fmap_set_path(const fmap_place_t *pts, int n, uint16_t c565, int width_px, uint8_t opa)
{
    if (!m.mu) return;
    lock();
    m.path_n = pts && n > 0 ? (n > FMAP_PATH_MAX ? FMAP_PATH_MAX : n) : 0;
    if (m.path_n) memcpy(m.path, pts, m.path_n * sizeof(*pts));
    m.path_c = c565;
    m.path_w = width_px;
    m.path_opa = opa;
    m.path_stamp++;
    int cur = m.t.cur1 - 1;
    bool shown = cur >= 0 && m.slot[cur].state != FMAP_LOADING && m.t.busy1 != cur + 1;
    if (shown) path_apply(cur);                           // the picture shown: updated at once
    unlock();
    if (shown) tell();
}
