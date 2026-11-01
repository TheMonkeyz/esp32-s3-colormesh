#pragma once
// forge_map's inside: what map.c (the task) and map_draw.c (the drawing both it and fmap_render use) share
#include "forge_map.h"

// A tile's body, in PSRAM: grown by doubling from BODY_MIN up to BODY_CAP (an OSM tile is 5-60 KB, rare dense ones
// ~100 KB; a body past the cap is refused, not cut). The task keeps it for a whole picture and frees it after.
#define FMAP_BODY_MIN (64 * 1024)
#define FMAP_BODY_CAP (256 * 1024)
typedef struct { uint8_t *buf; size_t len, cap; bool over, oom; } fmap_body_t;

// Called after each tile (drawn or not): ok tiles drawn so far, out of total
typedef void (*fmap_progress_t)(int ok, int total, void *user);

// Every tile of the window into dst (already cleared by the caller), on one keep-alive connection; svc_id < 0: not
// reported. body: the caller's (grown here; NULL buf on entry is fine; the caller frees buf). True when all arrived
// and nothing was cancelled; *ok_tiles and *total as they ended.
bool fmap_draw(const fmap_opts_t *o, int svc_id, int z, double ox, double oy, uint16_t *dst, int w, int h,
               fmap_body_t *body, bool (*cancel)(void *), void *cancel_user, fmap_progress_t progress, void *puser,
               int *ok_tiles, int *total);

// The flash cache's I/O over the "mapcache" partition (map_cache_flash.c, firmware only; weak NULL in map.c)
const fmap_cache_io_t *fmap_cache_flash_io(void (*pause)(void *ctx));

// The kept pictures' bookkeeping (map_slots.c, pure C; map.c holds it under its mutex, the pixels beside it)
typedef struct {
    int zoom;
    double ox, oy;
    uint32_t used;                     // stamp of the last time it was returned (the least recently used goes first)
    bool valid;                        // holds that view (its picture may still be loading)
} fmap_slot_key_t;

typedef struct {
    fmap_slot_key_t key[FMAP_SLOTS_MAX];
    int n;                             // slots in use, 1..FMAP_SLOTS_MAX
    int cur1, prev1;                   // the slot returned last and the (different) one before it, + 1 (0: none)
    int busy1;                         // the slot the task is drawing into, + 1 (set by map.c)
    uint32_t stamp;
} fmap_slots_t;

// The slot for the view (zoom, ox, oy): a valid one with that key (*hit true), else one to reuse (*hit false, its key
// set to the view, valid): an empty slot first, else the least recently used. Not the two returned last (with fewer
// than 3 slots, as many as leave one) unless nothing else is left; never the busy one. Stamps it and makes it the
// current one. -1: nothing can be reused (one slot, busy).
int fmap_slots_take(fmap_slots_t *t, int zoom, double ox, double oy, bool *hit);
