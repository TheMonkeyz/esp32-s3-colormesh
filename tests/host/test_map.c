// forge_map's pure half and its drawing against the scripted HTTP client (fake.c): Web Mercator maths checked against
// OpenStreetMap's own formulas (esp32-s3-rtcquebec's test_geo.c cases), places to a view and back, the tile cover at
// the world's edges (x wraps, y is cut), the tile URL with '%' in the template (L195) and when it doesn't fit, the
// dimming against weather_amoled's dim_map, the kept pictures' LRU with its protected slots, and fmap_render(): a real
// 4-bit OSM tile (osm_tile.h, L190) composed at a negative offset, a 404, retries, a body too big, a big one that
// fits, cancel, and the world's bottom edge at zoom 4.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "check.h"
#include "fake.h"
#include "freertos/task.h"
#include "osm_tile.h"
#include "png_rows.h"
#include "svc.h"
#include "map_priv.h"

/* ---------------- stubs for what map_draw.c calls ---------------- */

static int delays;                                       // vTaskDelay(1) calls: the decoder's pauses
static unsigned waited;                                  // longer ones (ms, the shim's ticks): before retries
void vTaskDelay(TickType_t t) { if (t == 1) delays++; else waited += t; }

static int last_svc = -2, svc_calls, last_why = -1;
const char *svc_user_agent(void) { return "stub-ua/1"; }
int svc_find(const char *name) { return name && !strcmp(name, "OpenStreetMap") ? 7 : -1; }
void svc_http(int id, esp_err_t err, int status, int64_t t0) { last_svc = id; svc_calls++; last_why = -1; }
void svc_fail_why(int id, svc_why_t code, int64_t t0) { last_svc = id; svc_calls++; last_why = code; }

/* ---------------- helpers ---------------- */

static uint8_t ref[256][256][4];                         // the OSM tile decoded alone (png_rows, test_png.c checks it)

static bool ref_row(unsigned y, const uint8_t *rgba, unsigned w, void *user)
{
    memcpy(ref[y], rgba, w * 4);
    return true;
}

// weather_amoled's dim_map, written out again here: the picture must match it at 55 % and half-way to grey
static uint16_t dim_map(uint8_t r, uint8_t g, uint8_t b)
{
    int l = (77 * r + 150 * g + 29 * b) >> 8;
    int R = ((l + r) / 2) * 55 / 100, G = ((l + g) / 2) * 55 / 100, B = ((l + b) / 2) * 55 / 100;
    return ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3);
}
static uint16_t ref_px(int x, int y) { return dim_map(ref[y][x][0], ref[y][x][1], ref[y][x][2]); }

// What the fake server answers: the tile at TILE_URL, or (for any URL) the tile when serve_all, else 404; fail_first
// transport errors or 503s before that
static const char *tile_url = "https://tile.openstreetmap.org/4/5/6.png";
static bool serve_all;
static int fail_first;
static esp_err_t fail_err;
static int fail_status;
static const uint8_t *big;                               // when set, every reply is this body
static size_t big_len;
static char urls[16][160];
static int nurls;

static fake_reply_t reply(const char *url, int n)
{
    if (nurls < 16) snprintf(urls[nurls++], sizeof(urls[0]), "%s", url);
    if (n < fail_first) return (fake_reply_t){ .err = fail_err, .status = fail_status };
    if (big) return (fake_reply_t){ .body = big, .len = big_len, .status = 200 };
    if (serve_all || !strcmp(url, tile_url)) return (fake_reply_t){ .body = osm_4bit, .len = sizeof(osm_4bit), .status = 200 };
    return (fake_reply_t){ .body = "Not Found", .len = 9, .status = 404 };
}

static void reset(void)
{
    fake_http = (fake_http_t){ .status = 200, .reply = reply };
    serve_all = false;
    fail_first = 0;
    fail_err = ESP_OK;
    fail_status = 0;
    big = NULL;
    nurls = 0;
    delays = 0;
    waited = 0;
    svc_calls = 0;
    last_svc = -2;
}

static bool seen(const char *url)
{
    for (int i = 0; i < nurls; i++) if (!strcmp(urls[i], url)) return true;
    return false;
}

// stderr (the ESP_LOG shim's) into a file for one call, to read the log line the harness parses
static int saved_fd = -1;
static FILE *cap;
static void capture_start(void)
{
    fflush(stderr);
    cap = tmpfile();
    saved_fd = dup(2);
    dup2(fileno(cap), 2);
}
static void capture_end(char *out, size_t n)
{
    fflush(stderr);
    dup2(saved_fd, 2);
    close(saved_fd);
    rewind(cap);
    size_t k = fread(out, 1, n - 1, cap);
    out[k] = 0;
    fclose(cap);
}

static int cancel_after, cancel_asked;
static bool cancel_cb(void *user)
{
    (*(int *)user)++;
    return ++cancel_asked > cancel_after;
}

/* ---------------- tests ---------------- */

static void test_geo(void)
{
    double x, y;
    // St-Dominique (stop 1025): 46.815845, -71.217659 at zoom 15 -> tile 9901, 11549 (the OSM wiki formula, in Python)
    geo_world_px(46.815845, -71.217659, 15, &x, &y);
    CHECK((int)(x / GEO_TILE) == 9901 && (int)(y / GEO_TILE) == 11549, "tile %d/%d", (int)(x / GEO_TILE), (int)(y / GEO_TILE));
    geo_world_px(0, 0, 0, &x, &y);
    CHECK(fabs(x - 128) < 1e-9 && fabs(y - 128) < 1e-9, "0,0 at zoom 0 is the centre: %f %f", x, y);
    geo_world_px(0, -180, 1, &x, &y);
    CHECK(fabs(x) < 1e-9 && fabs(y - 256) < 1e-9, "west edge: %f %f", x, y);
    geo_world_px(85.05112878, 0, 4, &x, &y);
    CHECK(fabs(y) < 1e-3, "85.0511 N is the world's top: %f", y);

    int tx0, ty0, tx1, ty1;
    geo_tiles(1000.0, 300.0, 466, 466, &tx0, &ty0, &tx1, &ty1);   // x 1000..1465 -> tiles 3..5; y 300..765 -> 1..2
    CHECK(tx0 == 3 && tx1 == 5 && ty0 == 1 && ty1 == 2, "%d-%d %d-%d", tx0, tx1, ty0, ty1);
    geo_tiles(512.0, 512.0, 256, 256, &tx0, &ty0, &tx1, &ty1);    // exactly one tile
    CHECK(tx0 == 2 && tx1 == 2 && ty0 == 2 && ty1 == 2, "%d-%d %d-%d", tx0, tx1, ty0, ty1);
    geo_tiles(-100.0, -1.0, 300, 2, &tx0, &ty0, &tx1, &ty1);      // left of and above the world: floor, not truncation
    CHECK(tx0 == -1 && tx1 == 0 && ty0 == -1 && ty1 == 0, "%d-%d %d-%d", tx0, tx1, ty0, ty1);

    // St-Dominique 1025 to 1105 (across Charest): a few tens of metres
    double d = geo_distance_m(46.815845, -71.217659, 46.81590, -71.21800);
    CHECK(d > 20 && d < 40, "%f m", d);
    CHECK(fabs(geo_distance_m(46.8, -71.2, 46.8, -71.2)) < 1e-6, "zero");
    double px = 300, py = 400;
    CHECK(geo_clamp_circle(&px, &py, 100) && fabs(px - 60) < 1e-9 && fabs(py - 80) < 1e-9, "%f %f", px, py);
    px = 30; py = 40;
    CHECK(!geo_clamp_circle(&px, &py, 100) && px == 30 && py == 40, "inside stays");

    // The origin: the same key both apps computed (rtcquebec floor(x - 466 / 2), weather_amoled floor(x) - 466 / 2)
    static const double places[][2] = { { 46.815845, -71.217659 }, { 45.5017, -73.5673 }, { -33.8688, 151.2093 },
                                        { 64.1466, -21.9426 }, { 0.0001, 0.0001 }, { -54.8019, -68.3030 } };
    for (int p = 0; p < 6; p++)
        for (int z = 2; z <= 19; z++) {
            double ox, oy, sx, sy;
            geo_world_px(places[p][0], places[p][1], z, &x, &y);
            geo_origin(places[p][0], places[p][1], z, 466, 300, &ox, &oy);
            CHECK(ox == floor(x - 233) && ox == floor(x) - 233 && oy == floor(y) - 150, "origin %d z%d: %f %f", p, z, ox, oy);
            // ...and back: the place is at the window's centre, within the pixel the floor dropped
            geo_to_view(places[p][0], places[p][1], z, ox, oy, &sx, &sy);
            CHECK(sx >= 233 && sx < 234 && sy >= 150 && sy < 151, "round trip %d z%d: %f %f", p, z, sx, sy);
        }
    // Across the 180th meridian: a view centred just west of it (its origin negative) shows a place just east of it
    // left of centre, not 8192 px away
    double ox, oy, sx, sy;
    geo_origin(0, -179.99, 5, 466, 466, &ox, &oy);
    geo_to_view(0, 179.99, 5, ox, oy, &sx, &sy);
    CHECK(ox == -233 && sx > 232 && sx < 233 && fabs(sy - 233) < 1, "antimeridian: ox %f, sx %f sy %f", ox, sx, sy);
    geo_origin(0, 179.99, 5, 466, 466, &ox, &oy);            // and the other way (x 8191.77: origin 7958, the
    geo_to_view(0, -179.99, 5, ox, oy, &sx, &sy);            //   place at 0.23 + 8192 - 7958)
    CHECK(ox == 8191 - 233 && sx > 234 && sx < 235, "antimeridian, from the east: ox %f, sx %f", ox, sx);

    // x wraps mod 2^z, also below 0
    CHECK(geo_wrap_x(-1, 4) == 15 && geo_wrap_x(16, 4) == 0 && geo_wrap_x(-17, 4) == 15 && geo_wrap_x(5, 4) == 5 &&
          geo_wrap_x(0, 0) == 0 && geo_wrap_x(-3, 0) == 0, "wrap %d %d %d %d", geo_wrap_x(-1, 4), geo_wrap_x(16, 4),
          geo_wrap_x(-17, 4), geo_wrap_x(5, 4));

    // The cover: negative tx kept (they wrap in the URL), y cut at the world's top and bottom
    geo_cover_t c;
    geo_cover(-100, -100, 466, 466, 4, &c);                  // x -100..365 -> -1..1; y -100..365 -> 0..1 (not -1)
    CHECK(c.tx0 == -1 && c.tx1 == 1 && c.ty0 == 0 && c.ty1 == 1 && c.count == 6, "top-left: %d-%d %d-%d = %d", c.tx0,
          c.tx1, c.ty0, c.ty1, c.count);
    geo_cover(1000, 4096 - 200, 466, 466, 4, &c);            // zoom 4's last row is 15: y 3896..4361 -> 15 only
    CHECK(c.ty0 == 15 && c.ty1 == 15 && c.tx0 == 3 && c.tx1 == 5 && c.count == 3, "bottom: %d-%d %d-%d = %d", c.tx0,
          c.tx1, c.ty0, c.ty1, c.count);
    geo_cover(1000, -1000, 466, 466, 4, &c);                 // wholly north of the world
    CHECK(c.count == 0, "north: %d", c.count);
    geo_cover(1000, 5000, 466, 466, 4, &c);                  // wholly south
    CHECK(c.count == 0, "south: %d", c.count);
    geo_cover(0, 0, 466, 466, 0, &c);                        // zoom 0: one tile, 2 columns of it (x wraps)
    CHECK(c.ty0 == 0 && c.ty1 == 0 && c.tx0 == 0 && c.tx1 == 1 && c.count == 2, "zoom 0: %d-%d = %d", c.tx0, c.tx1, c.count);
}

static void test_url(void)
{
    char u[FMAP_URL_MAX];
    CHECK(fmap_tile_url(u, sizeof(u), FMAP_OSM_URL, 15, 9901, 11549) &&
          !strcmp(u, "https://tile.openstreetmap.org/15/9901/11549.png"), "OSM: %s", u);
    // '%' copied as it is (an encoded '/' and a "%d" in a key); placeholders anywhere, twice; '{' alone and unknown
    // ones copied too
    const char *t = "https://t.example/{z}-{x}/{y}?k=%2F%d%s%&a={w}&b={z}{x}{";
    CHECK(fmap_tile_url(u, sizeof(u), t, 4, 15, 0) && !strcmp(u, "https://t.example/4-15/0?k=%2F%d%s%&a={w}&b=415{"),
          "verbatim: %s", u);
    // Cut: false, terminated, the prefix that fitted
    char s[20];
    memset(s, 'X', sizeof(s));
    CHECK(!fmap_tile_url(s, sizeof(s), FMAP_OSM_URL, 4, 5, 6) && !strcmp(s, "https://tile.openst"), "cut: %s", s);
    memset(s, 'X', sizeof(s));                               // cut in the middle of a number
    CHECK(!fmap_tile_url(s, 6, "ab{z}", 12345, 0, 0) && !strcmp(s, "ab123"), "cut in a number: %s", s);
    CHECK(fmap_tile_url(s, 6, "ab{z}", 123, 0, 0) && !strcmp(s, "ab123"), "exact fit: %s", s);
    CHECK(!fmap_tile_url(s, sizeof(s), NULL, 1, 2, 3) && s[0] == 0, "no template");
    CHECK(fmap_tile_url(s, sizeof(s), "{z}{", 1, 2, 3) && !strcmp(s, "1{"), "a '{' at the end: %s", s);
}

static void test_dim(void)
{
    int bad = 0;
    for (int r = 0; r < 256; r += 15)
        for (int g = 0; g < 256; g += 15)
            for (int b = 0; b < 256; b += 15) bad += fmap_dim565(r, g, b, 55, 50) != dim_map(r, g, b);
    CHECK(bad == 0, "55 %% / 50 %%: %d colours differ from weather_amoled's dim_map", bad);
    CHECK(fmap_dim565(255, 128, 8, 100, 0) == (((255 & 0xF8) << 8) | ((128 & 0xFC) << 3) | (8 >> 3)), "100/0: as is");
    CHECK(fmap_dim565(200, 30, 90, 100, 100) == fmap_dim565(85, 85, 85, 100, 0), "100 %% grey: its luma (85)");
}

static void take(fmap_slots_t *t, int z, double ox, int want, bool want_hit, const char *what)
{
    bool hit;
    int s = fmap_slots_take(t, z, ox, 0, &hit);
    CHECK(s == want && hit == want_hit, "%s: slot %d (hit %d), wanted %d (hit %d)", what, s, hit, want, want_hit);
}

static void test_slots(void)
{
    fmap_slots_t t = { .n = 4 };
    take(&t, 15, 100, 0, false, "first view");
    take(&t, 16, 200, 1, false, "zoom in");
    take(&t, 15, 100, 0, true, "back: kept");
    take(&t, 15, 300, 2, false, "empty slots first");
    take(&t, 15, 400, 3, false, "the last empty one");
    // Now LRU: used 0:3 1:2 2:4 3:5 -> slot 1
    take(&t, 17, 0, 1, false, "least recently used");
    CHECK(t.cur1 == 2 && t.prev1 == 4, "current %d, previous %d", t.cur1, t.prev1);
    // The busy slot is never reused, even when it is the least recently used (slot 0, used 3)
    t.busy1 = 1;
    take(&t, 17, 10, 2, false, "busy skipped");
    t.busy1 = 0;
    // A view twice in a row stays the current one, and the previous stays the previous
    take(&t, 17, 10, 2, true, "again");
    CHECK(t.cur1 == 3 && t.prev1 == 2, "again: current %d, previous %d", t.cur1, t.prev1);

    // 3 slots, the busy one and the two returned last: the older of the two goes, never the busy one
    fmap_slots_t u = { .n = 3 };
    take(&u, 4, 0, 0, false, "3: a");
    take(&u, 4, 1, 1, false, "3: b");
    take(&u, 4, 2, 2, false, "3: c");
    u.busy1 = 1;                                             // the task still drawing a (the least recently used)
    take(&u, 4, 3, 1, false, "3: busy and the two last: b goes");
    // 2 slots: only the last one is protected
    fmap_slots_t v = { .n = 2 };
    take(&v, 4, 0, 0, false, "2: a");
    take(&v, 4, 1, 1, false, "2: b");
    take(&v, 4, 0, 0, true, "2: a kept");
    take(&v, 4, 2, 1, false, "2: c replaces b");
    // 1 slot, busy: nothing to reuse (map.c reports the view failed); not busy: reused
    fmap_slots_t w = { .n = 1 };
    take(&w, 4, 0, 0, false, "1: a");
    w.busy1 = 1;
    take(&w, 4, 1, -1, false, "1: busy");
    take(&w, 4, 0, 0, true, "1: busy, but kept");
    w.busy1 = 0;
    take(&w, 4, 1, 0, false, "1: reused");
}

static void test_render(void)
{
    static uint16_t pic[600 * 300];
    fmap_opts_t o;
    fmap_opts_default(&o, 300, 200);
    unsigned tw = 0, th = 0;
    CHECK(png_rows(osm_4bit, sizeof(osm_4bit), ref_row, NULL, &tw, &th) && tw == 256 && th == 256, "the reference");
    char log[2048];
    int ok = -1;

    // 1. Tile 4/5/6 at (-100, -30) in a 300 x 200 picture: its right part and middle rows land, dimmed; tile 4/6/6
    //    is a 404 (not tried again); the picture's right part stays empty
    reset();
    capture_start();
    bool all = fmap_render(&o, 4, 5 * 256 + 100, 6 * 256 + 30, pic, 300, 200, NULL, NULL, &ok);
    capture_end(log, sizeof(log));
    CHECK(!all && ok == 1, "negative offset: all %d, ok %d", all, ok);
    CHECK(fake_http.requests == 2 && seen(tile_url) && seen("https://tile.openstreetmap.org/4/6/6.png"),
          "2 requests, the 404 once: %d", fake_http.requests);
    int bad_tile = 0, bad_empty = 0, dimmed = 0;
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 300; x++) {
            uint16_t p = pic[y * 300 + x];
            if (x < 156) {
                bad_tile += p != ref_px(x + 100, y + 30);
                const uint8_t *c = ref[y + 30][x + 100];
                dimmed += p != (((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3));
            } else bad_empty += p != o.empty565;
        }
    CHECK(bad_tile == 0, "tile pixels: %d differ from the reference, dimmed", bad_tile);
    CHECK(dimmed == 156 * 200, "dimmed: %d of %d pixels differ from the tile's own colour", dimmed, 156 * 200);
    CHECK(bad_empty == 0, "the 404's part: %d pixels not empty", bad_empty);
    CHECK(fake_http.inits == 1 && fake_http.cleanups == 1, "one kept connection: %d made, %d cleaned up",
          fake_http.inits, fake_http.cleanups);
    CHECK(!strcmp(fake_http.user_agent, "stub-ua/1"), "User-Agent: svc_user_agent(): %s", fake_http.user_agent);
    CHECK(last_svc == 7 && svc_calls == 2, "reported to svc_find(\"OpenStreetMap\"): id %d, %d calls", last_svc, svc_calls);
    // tile rows 0..230 decoded (230: the first below the picture, the decode stops): pauses at rows 63, 127, 191
    CHECK(delays == 3, "decoder pauses: %d", delays);
    CHECK(strstr(log, "I fmap: zoom 4 at 1380,1566: 1/2 tiles\n") != NULL, "the harness's log line: %s", log);

    // 2. A 503 then the tile: drawn on the second try; without retries it fails. A transport error drops the client.
    o.w = o.h = 256;
    reset();
    fail_first = 1;
    fail_status = 503;
    CHECK(fmap_render(&o, 4, 5 * 256, 6 * 256, pic, 256, 256, NULL, NULL, &ok) && ok == 1 && fake_http.requests == 2,
          "503 then 200: ok %d, %d requests", ok, fake_http.requests);
    CHECK(pic[0] == ref_px(0, 0) && pic[255 * 256 + 255] == ref_px(255, 255), "whole tile at (0, 0)");
    CHECK(delays == 4, "a whole tile: 4 pauses, %d", delays);
    CHECK(waited == 1000, "a second before the retry: %u ms", waited);
    reset();
    fail_first = 1;
    fail_status = 503;
    o.retries = 0;
    CHECK(!fmap_render(&o, 4, 5 * 256, 6 * 256, pic, 256, 256, NULL, NULL, &ok) && ok == 0 && fake_http.requests == 1,
          "no retries: ok %d, %d requests", ok, fake_http.requests);
    CHECK(pic[0] == o.empty565 && pic[256 * 256 - 1] == o.empty565, "failed: empty");
    o.retries = 2;
    reset();
    fail_first = 2;
    fail_err = ESP_ERR_HTTP_CONNECT;
    CHECK(fmap_render(&o, 4, 5 * 256, 6 * 256, pic, 256, 256, NULL, NULL, &ok) && ok == 1 && fake_http.requests == 3,
          "2 connect errors then 200: ok %d, %d requests", ok, fake_http.requests);
    CHECK(fake_http.inits == 3 && fake_http.cleanups == 3, "a client after each error: %d made, %d cleaned up",
          fake_http.inits, fake_http.cleanups);
    CHECK(waited == 1000 + 2000, "1 s, then 2 s before the retries: %u ms", waited);
    reset();                                                 // a 404 is not retried, whatever retries says
    tile_url = "none";
    CHECK(!fmap_render(&o, 4, 5 * 256, 6 * 256, pic, 256, 256, NULL, NULL, &ok) && fake_http.requests == 1 &&
          waited == 0, "404 with 2 retries: %d requests, %u ms waited", fake_http.requests, waited);
    tile_url = "https://tile.openstreetmap.org/4/5/6.png";

    // 3. Bodies: 300 KB is past the 256 KB cap (refused, not retried, reported); 200 KB (the tile, then zeros)
    //    grows the buffer 64 -> 128 -> 256 KB and is drawn
    uint8_t *b = calloc(300 * 1024, 1);
    memcpy(b, osm_4bit, sizeof(osm_4bit));
    reset();
    big = b;
    big_len = 300 * 1024;
    CHECK(!fmap_render(&o, 4, 5 * 256, 6 * 256, pic, 256, 256, NULL, NULL, &ok) && ok == 0 && fake_http.requests == 1,
          "too big: ok %d, %d requests", ok, fake_http.requests);
    CHECK(last_why == SVC_WHY_BAD_REPLY, "too big: reported as a bad reply (%d)", last_why);
    reset();
    big = b;
    big_len = 200 * 1024;
    CHECK(fmap_render(&o, 4, 5 * 256, 6 * 256, pic, 256, 256, NULL, NULL, &ok) && ok == 1 && pic[0] == ref_px(0, 0),
          "200 KB: ok %d", ok);
    big_len = 256 * 1024;                                    // exactly the cap: taken
    CHECK(fmap_render(&o, 4, 5 * 256, 6 * 256, pic, 256, 256, NULL, NULL, &ok) && ok == 1, "256 KB: ok %d", ok);
    free(b);

    // 4. Cancel: asked before each request; true at the third: 2 tiles drawn of 6, the rest empty
    reset();
    serve_all = true;
    cancel_after = 2;
    cancel_asked = 0;
    int asked = 0;
    o.user_agent = "test-ua/2";
    o.tile_url = "https://t.example/{z}/{x}/{y}.png?key=%2F%d%s";
    capture_start();
    all = fmap_render(&o, 4, 5 * 256, 6 * 256, pic, 600, 300, cancel_cb, &asked, &ok);   // 3 x 2 tiles
    capture_end(log, sizeof(log));
    CHECK(!all && ok == 2 && fake_http.requests == 2 && asked == 3, "cancel: ok %d, %d requests, asked %d", ok,
          fake_http.requests, asked);
    CHECK(pic[0] == ref_px(0, 0) && pic[300] == ref_px(44, 0) && pic[600 * 299] == o.empty565 &&
          pic[599] == o.empty565, "cancel: drawn tiles kept, the others empty");
    CHECK(strstr(log, "I fmap: zoom 4 at 1280,1536: cancelled at 2/6 tiles\n") != NULL, "cancel's log line: %s", log);
    CHECK(seen("https://t.example/4/5/6.png?key=%2F%d%s"), "the template's %% copied: %s", urls[0]);
    CHECK(!strcmp(fake_http.user_agent, "test-ua/2"), "the app's User-Agent: %s", fake_http.user_agent);

    // 5. The world's bottom-left corner at zoom 4: x -100..199 (tiles 15 and 0: wrapped), y 3996..4195 (row 15
    //    only; rows 16 aren't asked for, the picture below the world stays empty)
    reset();
    serve_all = true;
    o.tile_url = FMAP_OSM_URL;
    o.svc_name = NULL;
    CHECK(fmap_render(&o, 4, -100, 4096 - 100, pic, 300, 200, NULL, NULL, &ok) && ok == 2 && fake_http.requests == 2,
          "world edge: ok %d, %d requests", ok, fake_http.requests);
    CHECK(seen("https://tile.openstreetmap.org/4/15/15.png") && seen("https://tile.openstreetmap.org/4/0/15.png"),
          "world edge: wrapped x, last row: %s, %s", urls[0], urls[1]);
    CHECK(last_svc == -1, "no service name: not reported (%d)", last_svc);
    int bad = 0, below = 0;
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 300; x++) {
            if (y < 100) bad += pic[y * 300 + x] != ref_px((x + 156) % 256, y + 156);
            else below += pic[y * 300 + x] != o.empty565;
        }
    CHECK(bad == 0 && below == 0, "world edge: %d tile pixels wrong, %d below the world not empty", bad, below);

    // 6. Wrong arguments: nothing fetched
    reset();
    CHECK(!fmap_render(&o, 23, 0, 0, pic, 10, 10, NULL, NULL, &ok) && !fmap_render(&o, 4, 0, 0, pic, 0, 10, NULL, NULL, &ok)
          && !fmap_render(NULL, 4, 0, 0, pic, 10, 10, NULL, NULL, &ok) && fake_http.requests == 0, "bad arguments");
    o.tile_url = "https://t.example/{z}/{x}/{y}/" "0123456789012345678901234567890123456789012345678901234567890123456789"
                 "0123456789012345678901234567890123456789012345678901234567890123456789"
                 "0123456789012345678901234567890123456789012345678901234567890123456789"
                 "0123456789012345678901234567890123456789";
    CHECK(!fmap_render(&o, 4, 5 * 256, 6 * 256, pic, 256, 256, NULL, NULL, &ok) && fake_http.requests == 0,
          "a URL too long: not requested (%d)", fake_http.requests);
}

/* ---------------- the path overlay (map_path.c) ---------------- */

static void test_path(void)
{
    enum { PW = 64, PH = 48 };
    static uint16_t px[PW * PH], orig[PW * PH];
    static uint8_t mask[(PW * PH + 7) / 8];
    static fmap_undo_t undo[PW * PH];
    for (int i = 0; i < PW * PH; i++) px[i] = orig[i] = (uint16_t)(0x1000 + i);
    // A window at zoom 10 around Quebec City; two places on a diagonal through it
    int z = 10;
    double ox, oy, ax, ay, bx, by;
    geo_origin(46.81, -71.21, z, PW, PH, &ox, &oy);
    fmap_place_t pts[2] = { { 46.81, -71.21 }, { 46.81, -71.21 } };
    geo_to_view(pts[0].lat, pts[0].lon, z, ox, oy, &ax, &ay);    // the centre
    CHECK(fabs(ax - PW / 2) < 1.5 && fabs(ay - PH / 2) < 1.5, "the centre lands mid-window (%.1f, %.1f)", ax, ay);
    // The second place 20 px east and 10 px north of it: found by stepping the longitude / latitude
    pts[1].lon = pts[0].lon + 20.0 * 360.0 / (256.0 * (1 << z));
    pts[1].lat = pts[0].lat + 10.0 * 360.0 / (256.0 * (1 << z)) * 0.68;   // ~cos(lat) correction, near enough
    geo_to_view(pts[1].lat, pts[1].lon, z, ox, oy, &bx, &by);
    CHECK(bx > ax + 15 && by < ay - 5, "the second place is east and north (%.1f, %.1f)", bx, by);
    bool full = true;
    int changed = fmap_path_draw(px, PW, PH, z, ox, oy, pts, 2, 0xF800, 4, 255, mask, undo, PW * PH, &full);
    double len = hypot(bx - ax, by - ay);
    CHECK(changed > len * 3 && changed < len * 6 + 40, "a 4 px line %.0f px long changed %d pixels", len, changed);
    CHECK(!full, "the undo list had room");
    int at_mid = (int)lround((ay + by) / 2) * PW + (int)lround((ax + bx) / 2);
    CHECK(px[at_mid] == 0xF800, "the middle of the line is the path's colour at full opacity (0x%04x)", px[at_mid]);
    CHECK(px[2 * PW + 2] == orig[2 * PW + 2] && px[(PH - 2) * PW + PW - 2] == orig[(PH - 2) * PW + PW - 2],
          "far corners untouched");
    int outside = 0;
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
            // distance from the segment
            double t = ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / (len * len);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            double d = hypot(x - (ax + t * (bx - ax)), y - (ay + t * (by - ay)));
            if (px[y * PW + x] != orig[y * PW + x] && d > 3.5) outside++;
        }
    CHECK(outside == 0, "%d changed pixels lie further than the line's half width + 1.5", outside);
    fmap_path_undo(px, undo, changed);
    CHECK(!memcmp(px, orig, sizeof(px)), "undo puts every pixel back");
    // Half opacity blends; a one-point path is a dot; a tiny undo cap reports full
    CHECK(fmap_blend565(0x0000, 0xFFFF, 128) != 0 && fmap_blend565(0x0000, 0xFFFF, 128) != 0xFFFF, "a half blend is between");
    CHECK(fmap_blend565(0x1234, 0xFFFF, 0) == 0x1234 && fmap_blend565(0x1234, 0xABCD, 255) == 0xABCD, "opa 0 / 255");
    changed = fmap_path_draw(px, PW, PH, z, ox, oy, pts, 1, 0x07E0, 3, 255, mask, undo, 2, &full);
    CHECK(changed >= 5 && changed <= 13 && full, "a dot of 3 px: %d pixels, the undo cap of 2 reported full", changed);
    fmap_path_undo(px, undo, 2);
    CHECK(fmap_path_draw(NULL, PW, PH, z, ox, oy, pts, 2, 0, 1, 255, mask, NULL, 0, NULL) == 0, "no picture: nothing");
}

int main(void)
{
    test_path();
    test_geo();
    test_url();
    test_dim();
    test_slots();
    test_render();
    return check_done("map");
}
