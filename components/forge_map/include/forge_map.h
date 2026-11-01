#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "forge_geo.h"

// Street map pictures from map tiles (OpenStreetMap's by default): a w x h RGB565 picture of the map around a place at
// a zoom, desaturated and dimmed for a dark AMOLED screen, built from 256 px PNG tiles decoded a row at a time
// (forge_core png_rows: ~50 KB whatever the tile, every bit depth OSM sends, LESSONS L190). From esp32-s3-rtcquebec's
// map.c (v0.3.1) and weather_amoled's radar.c, which each had their own copy. No LVGL: the app shows px as an image.
//
// Two ways to use it:
//   - fmap_create() + fmap_set_center(): the component's own task downloads in the background into PSRAM pictures
//     ("slots") kept by (zoom, origin), so going back to a place or a zoom downloads nothing. set_center never waits
//     for the network; the picture fills in as tiles arrive (opts.updated is called after each one).
//   - fmap_render(): one picture, now, in the caller's task, into the caller's buffer (weather_amoled's alert map and,
//     later, its radar's base map), with its own connection. It uses no state of the task: both can run at once.
//
// Tile servers' rules (OSM's tile usage policy): an identifying User-Agent (svc_user_agent() by default), one
// connection kept alive, few requests (kept pictures; 404s are not retried), the attribution shown with the map.
//
// TODO(phase 2): a flash cache of whole pictures, weather_amoled's "MAP7" layout (one slot per zoom in a "mapcache"
// data partition, a header {magic, zoom, ox, oy} written last so it is valid only when complete, sectors erased and
// written one at a time with vTaskDelay(1) between them and paused while the screen is busy, read back in 4 KB pieces
// (a bigger read into PSRAM borrows internal RAM)). Not in phase 1: rtcquebec doesn't cache in flash, and weather
// keeps its own cache until then.

typedef enum { FMAP_LOADING, FMAP_READY, FMAP_FAILED } fmap_state_t;

typedef struct {
    int w, h;                          // the pictures' size in pixels (the task's slots; fmap_render takes its own)
    // The tile URL: "{z}", "{x}" and "{y}" are replaced by the tile's numbers (x already wrapped, forge_geo.h), every
    // other character is copied as it is, '%' included (an encoded query or key: never used as a printf format, L195).
    // At most FMAP_URL_MAX - 1 characters once filled in; a longer one fails that tile.
    const char *tile_url;
    const char *user_agent;            // NULL: forge_net's svc_user_agent()
    const char *attribution;           // the app shows it with the map (fmap_attribution()); the component shows nothing
    // The service the tile downloads are reported to (forge_net svc.h): fmap_create() adds it unless an app already
    // did (svc_find by name); fmap_render() only reports to one that exists. NULL name: not reported.
    const char *svc_name, *svc_api;
    int zoom_min, zoom_max;            // fmap_set_center() keeps the zoom within these (0..GEO_ZOOM_MAX)
    int retries;                       // tries after the first for a tile that failed (transport error, 5xx, 429),
                                       //   1 s, 2 s... after it; a 404 or a body too big is not tried again
    int dim_pct;                       // brightness kept, % (100: as the tiles are; rtcquebec and weather: 55)
    int desat_pct;                     // % of the way to grey (0: colours kept; rtcquebec and weather: 50)
    uint16_t empty565;                 // the colour where no tile is (yet), RGB565
    int psram_slots;                   // pictures kept (w x h x 2 bytes each, allocated on first use, never freed),
                                       //   1..FMAP_SLOTS_MAX; 3 or more keep the two returned last while a third loads
    void (*updated)(void *user);       // a tile was drawn or a picture is done: from the map task (take the display
    void *user;                        //   lock there); NULL: poll fmap_status()
    // Whole pictures of the home place (fmap_set_home) kept in a "mapcache" flash partition (type data, subtype 0x40,
    // one slot per zoom from zoom_min, 512 KB each at 466x466): the next boot shows them without a download. Sectors
    // are erased and written one at a time with a pause between them; busy (may be NULL) says when to wait longer (a
    // finger on the screen: forge_lvgl's slide_screen_busy). Without the partition nothing is cached.
    bool flash_cache;
    bool (*busy)(void);
} fmap_opts_t;

#define FMAP_URL_MAX 256               // with the User-Agent, under esp_http_client's 512-byte request head (L193)
#define FMAP_SLOTS_MAX 8
#define FMAP_OSM_URL "https://tile.openstreetmap.org/{z}/{x}/{y}.png"

// OpenStreetMap's standard tiles with esp32-s3-rtcquebec's choices: zoom 0..19, 1 retry, half-way to grey then 55 %
// (weather_amoled's dim_map), empty rgb565(18, 20, 24), 4 slots, attribution "© OpenStreetMap contributors",
// service "OpenStreetMap" / "Map tiles". Change what differs after the call.
void fmap_opts_default(fmap_opts_t *o, int w, int h);

typedef struct {
    const uint16_t *px;                // w x h RGB565, empty565 until tiles arrive; NULL if PSRAM was short. Stays
                                       //   valid until the slot is reused (the buffers are never freed): not while it
                                       //   is one of the two pictures set_center returned last (an app shows the old
                                       //   zoom until the new one is done), unless every other slot is the one the
                                       //   task is still drawing into
    int w, h;
    int zoom;
    double ox, oy;                     // its top-left corner in world pixels at that zoom (forge_geo.h)
    fmap_state_t state;
    int tiles, done;                   // tiles it needs (geo_cover), tiles drawn
} fmap_view_t;

// Copies opts (the strings are kept as pointers: literals or static), starts the task. Once; false if the options
// are unusable (size, zoom range, no URL) or the task couldn't start. The app needs
// CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY (the component's statics are in PSRAM, L185).
bool fmap_create(const fmap_opts_t *opts);

// The picture centred on (lat, lon) at zoom (kept within zoom_min..zoom_max): a kept one, or a new one the task
// starts downloading, the one shown first if several wait. A failed one is tried again. Never waits for the network
// (a new picture is cleared to empty565 here, ~1 ms per 100 K pixels). From any task.
void fmap_set_center(double lat, double lon, int zoom, fmap_view_t *out);
// What the last fmap_set_center()'s picture is now (state, tiles done); FMAP_FAILED with px NULL before the first
void fmap_status(fmap_view_t *out);
// opts.attribution ("" if none): the app shows it with the map, in its own font and place
const char *fmap_attribution(void);

// The place whose pictures the flash cache keeps (one view per zoom): the first place, the home. Before or after
// fmap_set_center; a picture already drawn for it is saved when it completes.
void fmap_set_home(double lat, double lon);

// A path over the task's pictures (a bus route, a track): places, drawn after the tiles of every picture as a line
// width_px wide in c565 at opa/255, into the picture itself: no second buffer, the pixels under it are kept so the
// path can be changed or removed without downloading again (the current picture is updated at once; a kept picture
// of another zoom gets the new path when it is shown again). At most FMAP_PATH_MAX places, copied. NULL or n 0: none.
#define FMAP_PATH_MAX 1024
typedef struct { double lat, lon; } fmap_place_t;
void fmap_set_path(const fmap_place_t *pts, int n, uint16_t c565, int width_px, uint8_t opa);

// One picture now, in the caller's task: dst (w x h RGB565) cleared to opts->empty565, then every tile of the window
// at (ox, oy) at zoom z fetched (opts' URL, User-Agent, retries) and drawn with opts' dimming. opts->w/h and the task's
// fields are not used. cancel (may be NULL) is asked before each request: true stops (the picture keeps what it has).
// *ok_tiles (may be NULL): tiles drawn. True when every tile arrived and nothing was cancelled. Several tasks may
// call it at once (each with its own connection and buffers).
bool fmap_render(const fmap_opts_t *opts, int z, double ox, double oy, uint16_t *dst, int w, int h,
                 bool (*cancel)(void *user), void *user, int *ok_tiles);

// ---- pure C, host-tested (tests/host/test_map.c) ----

// A tile URL template (fmap_opts_t.tile_url) filled in for tile (z, x, y) into out (n bytes). False if it didn't fit
// (out is then cut, still terminated) or there is no template.
bool fmap_tile_url(char *out, size_t n, const char *tmpl, int z, int x, int y);

// One pixel of a tile, as the picture shows it: desat % of the way to its grey (luma 77/150/29), then dim % of that
uint16_t fmap_dim565(uint8_t r, uint8_t g, uint8_t b, int dim_pct, int desat_pct);

// src over dst at opa/255 (RGB565 both)
uint16_t fmap_blend565(uint16_t dst, uint16_t src, uint8_t opa);
// The path into a w x h picture of zoom z at (ox, oy) (map_path.c): each pixel it touches is blended once (mask: w x h
// bits, (w * h + 7) / 8 bytes, zeroed here). undo (may be NULL) records each pixel's index and old colour, undo_cap at
// most; *undo_full (may be NULL) says the cap was reached (the rest is drawn, not recorded). Returns pixels changed.
typedef struct { uint32_t idx; uint16_t old; } fmap_undo_t;
int fmap_path_draw(uint16_t *px, int w, int h, int z, double ox, double oy, const fmap_place_t *pts, int n,
                   uint16_t c565, int width_px, uint8_t opa, uint8_t *mask, fmap_undo_t *undo, int undo_cap,
                   bool *undo_full);
void fmap_path_undo(uint16_t *px, const fmap_undo_t *undo, int n);   // the pixels back as they were

// The flash cache's layout and files, over an I/O table (map_cache.c, host-tested with a RAM-backed table): a slot per
// zoom from zoom_min, each round_up(16 + w * h * 2, 4096) bytes; a header {magic "MAP7", zoom, ox, oy} written last,
// so a slot is valid only when complete; reads in 4 KB pieces (a bigger read into PSRAM borrows internal RAM).
typedef struct {
    void *ctx;
    size_t size;                                                        // the partition's
    bool (*read)(void *ctx, size_t off, void *dst, size_t n);
    bool (*write)(void *ctx, size_t off, const void *src, size_t n);    // onto erased bytes
    bool (*erase)(void *ctx, size_t off, size_t n);                     // n sectors of 4096
    void (*pause)(void *ctx);                                           // between sectors (may be NULL)
} fmap_cache_io_t;
#define FMAP_CACHE_MAGIC 0x4D415037u                                    // "MAP7", weather_amoled's layout
size_t fmap_cache_slot_bytes(int w, int h);
bool fmap_cache_slot(const fmap_cache_io_t *io, int w, int h, int zoom, int zoom_min, size_t *off);   // false: no room
bool fmap_cache_hit(const fmap_cache_io_t *io, size_t off, int zoom, int32_t ox, int32_t oy);
bool fmap_cache_load(const fmap_cache_io_t *io, size_t off, int zoom, int32_t ox, int32_t oy, uint16_t *px, int w, int h);
bool fmap_cache_save(const fmap_cache_io_t *io, size_t off, int zoom, int32_t ox, int32_t oy, const uint16_t *px, int w, int h);

