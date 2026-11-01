#pragma once
#include <stdbool.h>
#include "lvgl.h"

// Moves drawn as pictures copied straight to the panel (~60 fps) instead of LVGL redrawing every widget for every
// frame (~25 fps for a simple full screen, 10-15 for a busy one: weather_amoled, docs/LESSONS.md "LVGL").
//
// - The picture of the screen shown follows the panel (every area LVGL flushes is copied into it), so a move never
//   waits for it. The current page's neighbours are rendered off-display while nobody touches (refreshed every 2 s),
//   so a drag's first frame comes ~15 ms after the 10 px that make it a drag; one not ready is rendered then
//   (lv_snapshot, ~30-40 ms). In-place changes and screen loads render the new picture when they start.
// - Pager drags: slide_pager() takes over a pager's finger drags. The drag is recognised in the touch read (before
//   LVGL acts on it), then followed outside LVGL: the neighbour page comes in under the finger, the ends resist and
//   bounce back, release past a third or a flick goes on, else back. Then pager_switch() and LVGL takes over again.
// - In-place changes (slide_change) and screen loads (slide_to): the old picture slides out, the new one in.
// Everything runs in the LVGL task with the display lock held. Without the board's panel hooks or PSRAM for two
// pictures, the same calls fall back to LVGL's own animation (or none). Log lines: "slide: ...".

void slide_init(void);                       // forge_lvgl_set_panel() calls it
void slide_pager(lv_obj_t *pager);           // up to 6 pagers; freezes LVGL's own scrolling of it
// A pager can sit on a page of another, across it (pager_on_view): a drag moves the pager on view along the drag's
// axis; with none that way it stays LVGL's (a list scrolling, a gesture). Neighbour pictures are kept for both
// (4 x 434 KB of PSRAM).
// The app changed what a neighbour page shows (the alerts next to a stop, after the stop changed): the kept pictures
// are old; re-rendered when idle, or when a drag reaches them first.
void slide_stale(void);
// A press became a pager drag (any pager given to slide_pager): hook(pager, user) is called once per drag, at the 10 px
// that decide it, before the first frame. The touch belongs to the drag from then on: LVGL sends the pressed object no
// PRESS_LOST and no RELEASED until after the move (a hold started on LV_EVENT_PRESSED ends here, not 0.4 s later).
// Called from the touch read (LVGL task, display lock held, before LVGL handles that read): set flags, no screen loads
// or object deletes. One hook; NULL removes it.
typedef void (*slide_drag_hook_t)(lv_obj_t *pager, void *user);
void slide_set_drag_hook(slide_drag_hook_t hook, void *user);
// The active screen changes in place: change(user) is called, then the old picture slides out towards -dir (dir 1:
// the new state comes in from the right / below). For pages that are one screen with different contents.
void slide_change(int dir, bool vertical, void (*change)(void *user), void *user);
void slide_to(lv_obj_t *scr, int dir, bool vertical);   // lv_screen_load with a slide; dir 1: from the right / below
bool slide_busy(void);                       // a move is queued or running
// A tap on a page right after a move can be the next quick swipe's press: it reached LVGL as a short click and
// opened esp32-s3-rtcquebec's map (harness quick_swipes, October 7). An app's click handler on a pager page asks
// slide_tap_ok() first: false while a move runs and for SLIDE_TAP_GUARD_MS after one ended.
#define SLIDE_TAP_GUARD_MS 600
bool slide_tap_ok(void);
