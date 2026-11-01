// Tiles into a picture (forge_map.h): the URL, the download on a kept connection, the row-by-row decode and the
// dimming. Used by the map task (map.c) and by fmap_render() in any task: no state of its own, everything is the
// caller's. Built for the host too (tests/host/test_map.c against the scripted HTTP client in tests/host/fake.c).
#include "map_priv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "png_rows.h"
#include "svc.h"

static const char *TAG = "fmap";

void fmap_opts_default(fmap_opts_t *o, int w, int h)
{
    *o = (fmap_opts_t){
        .w = w, .h = h, .tile_url = FMAP_OSM_URL, .attribution = "\xC2\xA9 OpenStreetMap contributors",
        .svc_name = "OpenStreetMap", .svc_api = "Map tiles",
        .zoom_min = 0, .zoom_max = 19,                    // OSM's standard style stops at 19
        .retries = 1,                                     // rtcquebec: 2 tries a tile
        .dim_pct = 55, .desat_pct = 50,                   // weather_amoled's dim_map, rtcquebec's too
        .empty565 = 0x10A3,                               // rgb565(18, 20, 24): dark, a shade above black
        .psram_slots = 4,                                 // rtcquebec: a stop at two zooms, and another
    };
}

// Copies n bytes of s to out at *k, as far as there is room (one byte kept for the terminator); false if cut
static bool put(char *out, size_t n, size_t *k, const char *s, size_t len)
{
    size_t room = n - 1 - *k, c = len < room ? len : room;
    memcpy(out + *k, s, c);
    *k += c;
    return c == len;
}

bool fmap_tile_url(char *out, size_t n, const char *tmpl, int z, int x, int y)
{
    if (!out || !n) return false;
    out[0] = 0;
    if (!tmpl) return false;
    size_t k = 0;
    bool fit = true;
    // The template is copied piece by piece, never handed to printf as its format: an encoded "%2F" or a key holding
    // "%d" would be read as a conversion (L195). Only the three placeholders are numbers, printed with a literal "%d".
    for (const char *p = tmpl; *p && fit; ) {
        int v = 0;
        bool ph = p[0] == '{' && p[1] && p[2] == '}' && (p[1] == 'z' || p[1] == 'x' || p[1] == 'y');
        if (ph) {
            v = p[1] == 'z' ? z : p[1] == 'x' ? x : y;
            char num[12];
            int len = snprintf(num, sizeof(num), "%d", v);
            fit = put(out, n, &k, num, (size_t)len);
            p += 3;
        } else {
            const char *q = p + 1;                        // up to the next '{' (or the end): one copy
            while (*q && *q != '{') q++;
            fit = put(out, n, &k, p, (size_t)(q - p));
            p = q;
        }
    }
    out[k] = 0;
    return fit;
}

static inline uint16_t rgb565(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

uint16_t fmap_dim565(uint8_t r, uint8_t g, uint8_t b, int dim_pct, int desat_pct)
{
    // OSM's standard style is bright: desat % of the way to its grey, then dim % of that. At 50 and 55 this is
    // weather_amoled's dim_map exactly (((l + c) / 2) * 55 / 100), so its cached maps and rtcquebec's look the same.
    int l = (77 * r + 150 * g + 29 * b) >> 8, keep = 100 - desat_pct;
    int R = (l * desat_pct + r * keep) / 100 * dim_pct / 100;
    int G = (l * desat_pct + g * keep) / 100 * dim_pct / 100;
    int B = (l * desat_pct + b * keep) / 100 * dim_pct / 100;
    return rgb565(R, G, B);
}

/* ---------------- the download ---------------- */

static esp_err_t on_http(esp_http_client_event_t *e)
{
    fmap_body_t *b = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || b->over) return ESP_OK;
    size_t need = b->len + (size_t)e->data_len;
    if (need > b->cap) {                                  // grow by doubling, from 64 KB up to the cap
        size_t nc = b->cap ? b->cap : FMAP_BODY_MIN;
        while (nc < need && nc < FMAP_BODY_CAP) nc *= 2;
        if (nc > FMAP_BODY_CAP) nc = FMAP_BODY_CAP;
        // Past the cap: the rest of the body is read and dropped (the client ignores a handler's error), so the kept
        // connection stays in step; the tile is refused, not drawn cut
        if (need > nc) { b->over = true; return ESP_OK; }
        uint8_t *nb = heap_caps_realloc(b->buf, nc, MALLOC_CAP_SPIRAM);
        if (!nb) { b->over = b->oom = true; return ESP_OK; }
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, e->data, (size_t)e->data_len);
    b->len = need;
    return ESP_OK;
}

typedef enum { GOT, FAIL_RETRY, FAIL_FINAL } got_t;

// One tile on the kept connection: *hc made on first use, dropped after a transport error (the next request starts
// a fresh one; a reused client after a failed TLS read fails again)
static got_t fetch(esp_http_client_handle_t *hc, const fmap_opts_t *o, int svc_id, const char *url, fmap_body_t *b,
                   int z, int tx, int ty)
{
    b->len = 0;
    b->over = b->oom = false;
    if (!*hc) {
        esp_http_client_config_t c = {
            .url = url, .event_handler = on_http, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000,
            .user_agent = o->user_agent ? o->user_agent : svc_user_agent(), .buffer_size = 4096,
            .keep_alive_enable = true,
        };
        if (!(*hc = esp_http_client_init(&c))) {           // memory short: perform() would dereference NULL
            ESP_LOGW(TAG, "tile %d/%d/%d: no memory for a client", z, tx, ty);
            return FAIL_RETRY;
        }
    } else {
        esp_http_client_set_url(*hc, url);
    }
    esp_http_client_set_user_data(*hc, b);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(*hc);
    int st = esp_http_client_get_status_code(*hc);
    if (err == ESP_OK && st == 200 && b->len == 0) svc_fail_why(svc_id, SVC_WHY_EMPTY, t0);
    else if (err == ESP_OK && st == 200 && b->over && !b->oom) svc_fail_why(svc_id, SVC_WHY_BAD_REPLY, t0);
    else svc_http(svc_id, err, st, t0);
    if (err != ESP_OK) { esp_http_client_cleanup(*hc); *hc = NULL; }
    if (err == ESP_OK && st == 200 && b->len > 0 && !b->over) return GOT;
    // The tile's numbers, not its URL: a template may hold a key
    ESP_LOGW(TAG, "tile %d/%d/%d: %s, HTTP %d%s", z, tx, ty, esp_err_to_name(err), st,
             b->oom ? " (no memory for the body)" : b->over ? " (too big)" : st == 200 && !b->len ? " (empty)" : "");
    // Worth another try: the network, the server busy (5xx, 429 too many requests), memory; not a 404 or a body too big
    bool again = err != ESP_OK || st >= 500 || st == 429 || b->oom || (st == 200 && b->len == 0);
    return again ? FAIL_RETRY : FAIL_FINAL;
}

/* ---------------- the decode ---------------- */

typedef struct { uint16_t *dst; int w, h, x, y, dim, desat; } tile_t;   // a 256 px tile into dst at (x, y)

static bool tile_row(unsigned y, const uint8_t *rgba, unsigned tw, void *user)
{
    const tile_t *t = user;
    if ((y & 63) == 63) vTaskDelay(1);                    // ~10 ms of decoding: let the other tasks of the core run
    int sy = t->y + (int)y;
    if (sy < 0) return true;                              // above the picture
    if (sy >= t->h) return false;                         // below it: done (png_rows counts that as decoded)
    int x0 = t->x < 0 ? -t->x : 0, x1 = t->w - t->x < (int)tw ? t->w - t->x : (int)tw;
    uint16_t *row = t->dst + (size_t)sy * t->w;           // (x0..x1: the tile's columns that land in the picture)
    for (int x = x0; x < x1; x++)
        row[t->x + x] = fmap_dim565(rgba[x * 4], rgba[x * 4 + 1], rgba[x * 4 + 2], t->dim, t->desat);
    return true;
}

bool fmap_draw(const fmap_opts_t *o, int svc_id, int z, double ox, double oy, uint16_t *dst, int w, int h,
               fmap_body_t *body, bool (*cancel)(void *), void *cancel_user, fmap_progress_t progress, void *puser,
               int *ok_tiles, int *total)
{
    geo_cover_t c;
    geo_cover(ox, oy, w, h, z, &c);
    int ok = 0, tries = 1 + (o->retries > 0 ? o->retries : 0);
    bool stopped = false;
    char url[FMAP_URL_MAX];
    esp_http_client_handle_t hc = NULL;
    *total = c.count;
    for (int ty = c.ty0; ty <= c.ty1 && !stopped; ty++)
        for (int tx = c.tx0; tx <= c.tx1 && !stopped; tx++) {
            int wx = geo_wrap_x(tx, z);                   // the URL's column; the tile lands where tx says
            got_t got = FAIL_FINAL;
            if (!fmap_tile_url(url, sizeof(url), o->tile_url, z, wx, ty))
                ESP_LOGW(TAG, "tile %d/%d/%d: URL longer than %d", z, wx, ty, FMAP_URL_MAX - 1);
            else
                for (int a = 0; a < tries; a++) {
                    // A second (then two...) before trying again: a busy server isn't asked again at once
                    if (a) vTaskDelay(pdMS_TO_TICKS(1000 * a));
                    if (cancel && cancel(cancel_user)) { stopped = true; break; }
                    if ((got = fetch(&hc, o, svc_id, url, body, z, wx, ty)) != FAIL_RETRY) break;
                }
            if (stopped) break;
            tile_t t = { dst, w, h, tx * GEO_TILE - (int)floor(ox), ty * GEO_TILE - (int)floor(oy), o->dim_pct,
                         o->desat_pct };
            if (got == GOT && png_rows(body->buf, body->len, tile_row, &t, NULL, NULL)) ok++;
            else if (got == GOT) ESP_LOGW(TAG, "tile %d/%d/%d: not a PNG it can read", z, wx, ty);
            if (progress) progress(ok, c.count, puser);
        }
    if (hc) esp_http_client_cleanup(hc);
    *ok_tiles = ok;
    // The harness reads this line (and esp32-s3-rtcquebec's "map: zoom ..." before it): keep its format
    if (stopped) ESP_LOGI(TAG, "zoom %d at %.0f,%.0f: cancelled at %d/%d tiles", z, ox, oy, ok, c.count);
    else ESP_LOGI(TAG, "zoom %d at %.0f,%.0f: %d/%d tiles", z, ox, oy, ok, c.count);
    return !stopped && ok == c.count;
}

bool fmap_render(const fmap_opts_t *opts, int z, double ox, double oy, uint16_t *dst, int w, int h,
                 bool (*cancel)(void *user), void *user, int *ok_tiles)
{
    int ok = 0, total = 0;
    if (ok_tiles) *ok_tiles = 0;
    if (!opts || !dst || w <= 0 || h <= 0 || z < 0 || z > GEO_ZOOM_MAX) return false;
    for (size_t i = 0; i < (size_t)w * h; i++) dst[i] = opts->empty565;
    fmap_body_t b = { 0 };
    bool all = fmap_draw(opts, opts->svc_name ? svc_find(opts->svc_name) : -1, z, ox, oy, dst, w, h, &b, cancel, user,
                         NULL, NULL, &ok, &total);
    free(b.buf);                                          // up to 256 KB of PSRAM back at once
    if (ok_tiles) *ok_tiles = ok;
    return all;
}
