#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "web.h"

// Named screens, for the test console ("screen" = which one is shown, "screen <name>" = show it) and for
// GET /api/snapshot?screen=<name> (a picture of a screen rendered off-display, even one not shown). Names match
// forge.json "screens" (the harness snapshots each).
//
// get: the screen's object (an lv_screen, or a full-size object on one: an overlay). show: make it the one on the
// display (NULL: lv_screen_load(get())). prepare: optional, called before a snapshot of a screen not shown, so its
// texts are up to date (the app's refresh). shown: optional, says whether it is the one shown (pages of a pager
// share one screen); without it, the active screen or a visible object on it counts. All are called with the LVGL
// lock held.
typedef struct {
    const char *name;
    lv_obj_t *(*get)(void);
    void (*show)(void);
    void (*prepare)(void);
    bool (*shown)(void);
} screen_def_t;

void screens_register(const screen_def_t *defs, int n);   // static array; register overlays after their screen
const char *screens_current(void);                        // name of what's shown, "other" if none matches

// For web_set_snapshot(screens_snapshot, screens_snapshot_free): "current" or a registered name
bool screens_snapshot(const char *name, web_image_t *out);
void screens_snapshot_free(web_image_t *img);
