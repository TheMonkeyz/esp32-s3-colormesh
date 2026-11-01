#pragma once
#include <stdbool.h>
#include "lvgl.h"

// Moves drawn as pictures copied straight to the panel (~60 fps) instead of LVGL redrawing every widget for every
// frame (~25 fps for a simple full screen, 10-15 for a busy one: weather_amoled, docs/LESSONS.md "LVGL").
//
// - The picture of the screen shown follows the panel (every area LVGL flushes is copied into it), so a move never
//   waits for it. The current page's neighbours are rendered off-display while nobody touches (refreshed every 2 s),
//   so a drag's first frame comes ~15 ms after the DRAG_PX that make it a drag; one not ready is rendered then
//   (lv_snapshot, ~30-40 ms). In-place changes and screen loads render the new picture when they start.
// - Pager drags: slide_pager() takes over a pager's finger drags. The drag is recognised in the touch read (before
//   LVGL acts on it), then followed outside LVGL: the neighbour page comes in under the finger, the ends resist and
//   bounce back, release past a third or a flick goes on, else back. Then pager_switch() and LVGL takes over again.
//   DRAG_PX is Kconfig FORGE_SLIDE_DRAG_PX (menu "espforge LVGL", 10 by default: LVGL's own; 28 when a tap or a hold
//   with a slight finger move became a swipe, esp32-s3-colormesh). List scrolls are decided at 10 px whatever it is.
// - In-place changes (slide_change) and screen loads (slide_to): the old picture slides out, the new one in.
// Everything runs in the LVGL task with the display lock held. Without the board's panel hooks or PSRAM for two
// pictures, the same calls fall back to LVGL's own animation (or none). Log lines: "slide: ...". Test console:
// "slide" answers "test: slide drag_px=N scroll_px=N lvgl_px=N drags=N scrolls=N hooked=N dropped=N" (since boot);
// "slide hold X Y MS X2 Y2 [MOVE_MS [END_MS]]" holds a simulated finger, then moves it on.

void slide_init(void);                       // forge_lvgl_set_panel() calls it
void slide_pager(lv_obj_t *pager);           // up to 6 pagers; freezes LVGL's own scrolling of it
// A pager can sit on a page of another, across it (pager_on_view): a drag moves the pager on view along the drag's
// axis; with none that way it stays LVGL's (a list scrolling, a gesture). Neighbour pictures are kept for both: up to
// 4 x 434 KB of PSRAM, allocated when a picture needs one and PSRAM has room (slide_slots.c's rules, from MeteoBus).
// The app changed what a neighbour page shows (the alerts next to a stop, after the stop changed): the kept pictures
// are old; re-rendered when idle, or when a drag reaches them first.
void slide_stale(void);
// A press became a pager drag (any pager given to slide_pager): hook(pager, user) is called once per drag, at the
// DRAG_PX that decide it, before the first frame. The touch belongs to the drag from then on: LVGL sends the pressed
// object no PRESS_LOST and no RELEASED, during the move or after it (a hold started on LV_EVENT_PRESSED ends here, not
// at the next tick that notices). Called from the touch read (LVGL task, display lock held, before LVGL handles that
// read): set flags, log, no screen loads or object deletes, nothing slow (the drag's first frame waits). One hook;
// NULL removes it. From esp32-s3-colormesh.
typedef void (*slide_drag_hook_t)(lv_obj_t *pager, void *user);
// Every band of a pager move's frames, once composed (panel byte order, `n` rows of the screen's width from row `y0`),
// before it goes to the panel: the app may paint over it, e.g. an animation behind the pages in their pure-black
// pixels, so it keeps moving during a swipe. `pager` and `cur` (its page shown when the move started) and `off` (where
// that page is now: < 0 moved left / up, the next page shows; > 0 moved right / down, the previous one) say which page
// each pixel belongs to. LVGL task, display lock held; called for drags and the ease after the release, at ~60 fps:
// keep it fast or return at once. One hook; NULL removes it. From esp32-s3-colormesh (waves during swipes).
typedef void (*slide_frame_hook_t)(int y0, int n, uint16_t *dst, lv_obj_t *pager, int cur, int off, bool vertical,
                                   void *user);
void slide_set_frame_hook(slide_frame_hook_t hook, void *user);
void slide_set_drag_hook(slide_drag_hook_t hook, void *user);
// The app took the touch now down (a long press acted): the rest of it is ignored until the finger lifts: no pager
// drag, no list scroll, and LVGL waits for the release too (lv_indev_wait_release: no scroll of its own, no CLICKED on
// the lift; the pressed object gets PRESS_LOST then). From an LVGL event handler (LVGL task, display lock held); no
// effect without a touch down. Without it, a finger that moved on after a long press still started a drag (it was
// still tracked). Log: "slide: touch dropped ...".
void slide_drop_touch(void);
// PSRAM the pictures leave to the app (KB): a neighbour's buffer is allocated only while more than this stays free
#define SLIDE_PSRAM_RESERVE_KB 1000
// A screen that needs PSRAM for a while (a map with its tiles): free the pictures of pages not wanted now (not the
// neighbours of a pager on view). With the display lock held. They come back when idle, once the screen is left.
void slide_release_unneeded(void);
// The finger now held is where it came down (within max_px). LVGL fires LONG_PRESSED after 400 ms however far the
// finger moved when no scroll took over (a swipe on a screen without a pager, a late read on a slow page: L200). A
// long-press handler asks this first; SLIDE_HOLD_PX is esp32-s3-rtcquebec's rule.
#define SLIDE_HOLD_PX 24
bool slide_held_still(int max_px);
// The active screen changes in place: change(user) is called, then the old picture slides out towards -dir (dir 1:
// the new state comes in from the right / below). For pages that are one screen with different contents.
void slide_change(int dir, bool vertical, void (*change)(void *user), void *user);
void slide_to(lv_obj_t *scr, int dir, bool vertical);   // lv_screen_load with a slide; dir 1: from the right / below
bool slide_busy(void);                       // a move is queued or running
// A move runs or a finger touched the screen within 500 ms: a long flash write (forge_map's cache) waits
bool slide_screen_busy(void);
// A vertical drag on a scrollable list with no pager that way (the Settings screen) is drawn as pictures too: the rows
// moved within the shadow, the rows coming in rendered, only the list's rectangle sent (the board's display_raw_area;
// without it, LVGL scrolls the list itself at ~25 fps). Log: "slide: scroll: N frames in T ms, ...".
// A tap on a page right after a move can be the next quick swipe's press: it reached LVGL as a short click and
// opened esp32-s3-rtcquebec's map (harness quick_swipes, October 7). An app's click handler on a pager page asks
// slide_tap_ok() first: false while a move runs and for SLIDE_TAP_GUARD_MS after one ended.
#define SLIDE_TAP_GUARD_MS 600
bool slide_tap_ok(void);
