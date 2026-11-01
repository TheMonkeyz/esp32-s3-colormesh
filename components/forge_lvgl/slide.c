// Moves drawn as pictures (see slide.h). Ported and trimmed from weather_amoled's slide.c (v1.12.1): the frame
// composition, the finger rules and the drag loop are the same; its picture cache is replaced by a shadow of the panel
// plus the current page's neighbours, kept ready while nobody touches (rendered when a drag starts only if not ready).
#include "slide.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "forge_lvgl.h"
#include "pager.h"
#include "slide_slots.h"
#include "testcon.h"
#include "indev/lv_indev_private.h"         // wait_until_release: cleared after a drag (touch_resync)
#include "lvgl_private.h"                   // list scrolls: layers, the redraw list (inv_p)

static const char *TAG = "slide";
static const forge_panel_t *P;
static int W, H;
static volatile int phase;                  // breadcrumb for the test console's "where"

/* ---------- pictures ---------- */

static lv_draw_buf_t *shadow;               // what the panel shows (every LVGL flush is copied into it)
static lv_draw_buf_t *other;                // the picture coming in, rendered when a move starts
/* The neighbours of the page shown, kept ready: rendered while a drag waited for them (35-45 ms), the page started late
 * and jumped to catch the finger up (the user's "hiccups", October 4). Rendered by the idle timer, one per tick, when
 * nobody has touched the screen for IDLE_MS; re-rendered when older than STALE_MS (contents change: a clock, a status
 * line), so a drag shows at most 2 s old contents for the length of the drag (LVGL redraws the real page after).
 * NB slots, each holding the picture of one page: prev and next of every pager on view (two with a pager on a page of
 * another: up to four). slide_stale() marks them all old (the app changed what a neighbour shows).
 * Their 434 KB buffers are allocated when a picture needs one and PSRAM has room (room_for), never all at start (until
 * v0.6.0 the four were taken at init: 1.7 MB an app with two pages never used). Which slot a picture takes follows
 * MeteoBus's rules (slide_slots.c, host-tested): empty first, then an unwanted page, a wanted one only when worth less
 * than the new picture or when a drag needs its picture now; with no room for another buffer, a less useful picture's
 * buffer is reused. slide_release_unneeded() frees the buffers of pages not wanted now (a memory-hungry screen on top). */
#define IDLE_MS 300
#define STALE_MS 2000
#define NB 4
static struct { lv_draw_buf_t *buf; lv_obj_t *page; uint32_t at, used; } nb[NB];
static uint32_t touched_ms;                 // lv_tick of the last press seen
static bool shadow_ok;                      // a whole frame has been flushed since the shadow was (re)started

static void flushed(const lv_area_t *a, const uint8_t *px)
{
    int w = a->x2 - a->x1 + 1, x1 = a->x1 < 0 ? 0 : a->x1, x2 = a->x2 > W - 1 ? W - 1 : a->x2;
    for (int y = a->y1 < 0 ? 0 : a->y1; y <= a->y2 && y < H; y++)
        memcpy(shadow->data + y * shadow->header.stride + x1 * 2, px + ((y - a->y1) * w + x1 - a->x1) * 2,
               (x2 - x1 + 1) * 2);
    if (a->x1 <= 0 && a->y1 <= 0 && a->x2 >= W - 1 && a->y2 >= H - 1) shadow_ok = true;
}

static void rendered(lv_event_t *e) { if (!shadow_ok && lv_display_get_default()) shadow_ok = true; }

// An object rendered off-display into `other` (its own coordinates: a pager page off screen works too)
static bool render(lv_obj_t *obj)
{
    lv_obj_update_layout(lv_obj_get_screen(obj));
    return lv_snapshot_take_to_draw_buf(obj, LV_COLOR_FORMAT_RGB565, other) == LV_RESULT_OK;
}

/* ---------- frames ---------- */

typedef struct {
    const uint8_t *cur, *prev, *next;   // pictures (RGB565): prev = left / above, next = right / below; NULL = black
    uint32_t stride;                    // bytes per picture row (both pictures alike)
    bool vertical;
    int off;                            // where the current picture is: 0 = in place, < 0 moved left / up (next shows),
                                        // > 0 moved right / down (prev shows); even
    lv_obj_t *pager;                    // a pager move: its page shown at the start (for slide_frame_hook), else NULL
    int page;
} frame_t;

static slide_frame_hook_t frame_hook;                         // slide_set_frame_hook
static void *frame_hook_user;
void slide_set_frame_hook(slide_frame_hook_t hook, void *user) { frame_hook = hook; frame_hook_user = user; }

// n pixels, little-endian RGB565 -> panel byte order, two at a time (rows and offsets are even, so 4-byte aligned)
static inline void copy_swap(void *dst, const void *src, int n)
{
    uint32_t *d = dst;
    if (!src) { for (int i = 0; i < n / 2; i++) d[i] = 0; return; }
    const uint32_t *s = src;
    for (int i = 0; i < n / 2; i++) {
        uint32_t v = s[i];
        d[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
}

static inline const uint8_t *row(const uint8_t *pic, int y, uint32_t stride) { return pic ? pic + y * stride : NULL; }
static inline const uint8_t *at(const uint8_t *r, int x) { return r ? r + x * 2 : NULL; }

static void fill_rows(int y0, int n, void *dst, const frame_t *f);

static void fill(int y0, int n, void *dst, void *user)
{
    const frame_t *f = user;
    fill_rows(y0, n, dst, f);
    slide_frame_hook_t hook = frame_hook;
    if (hook && f->pager) hook(y0, n, dst, f->pager, f->page, f->off, f->vertical, frame_hook_user);
}

static void fill_rows(int y0, int n, void *dst, const frame_t *f)
{
    uint8_t *d = dst;
    const int o = f->off;
    for (int y = y0; y < y0 + n; y++, d += W * 2) {
        if (!f->vertical) {                              // a row = the end of one picture + the start of the other
            const uint8_t *c = row(f->cur, y, f->stride);
            if (o <= 0) {
                int a = -o;
                copy_swap(d, at(c, a), W - a);
                copy_swap(d + (W - a) * 2, row(f->next, y, f->stride), a);
            } else {
                copy_swap(d, at(row(f->prev, y, f->stride), W - o), o);
                copy_swap(d + o * 2, c, W - o);
            }
        } else if (o <= 0) {                             // a row of one picture or the other
            int sy = y - o;
            copy_swap(d, sy < H ? row(f->cur, sy, f->stride) : row(f->next, sy - H, f->stride), W);
        } else {
            copy_swap(d, y < o ? row(f->prev, H - o + y, f->stride) : row(f->cur, y - o, f->stride), W);
        }
    }
}

static void show(frame_t *f, int off)
{
    f->off = off & ~1;
    P->raw_frame(fill, f);
}

// From the current offset to `to`, ease out (cubic), frames as fast as they go
static int animate(frame_t *f, int to, int ms)
{
    int from = f->off, frames = 0;
    int64_t start = esp_timer_get_time();
    for (;;) {
        int64_t el = esp_timer_get_time() - start;
        float t = el >= ms * 1000LL ? 1.0f : (float)el / (ms * 1000.0f);
        float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        show(f, from + (int)((to - from) * e));
        frames++;
        if (t >= 1.0f) return frames;
    }
}

/* ---------- the finger ----------
 * While a move runs, LVGL is paused and never sees the finger lift. The touch that started it was told to wait for its
 * release (lv_indev_wait_release), so LVGL took the next touch for the same one: quick successive swipes were missed.
 * After the move: LVGL starts afresh, and a finger already down counts as a new press. */
// The press read_hook is tracking (a pager drag in the making); p0 where it came down, p where it is. dropped: the app
// took this touch (slide_drop_touch), nothing of it is ours until the finger lifts.
static struct { bool down, armed, dropped; lv_point_t p0, p; lv_obj_t *pager; int64_t t0; } track;
// Since boot, for the test console's "slide": pager drags started, drag hook calls, touches dropped, list scrolls
static volatile uint32_t n_drags, n_hooked, n_dropped, n_scrolls;

bool slide_held_still(int max_px)
{
    return !track.down || (abs(track.p.x - track.p0.x) <= max_px && abs(track.p.y - track.p0.y) <= max_px);
}

static void touch_resync(bool lifted)
{
    int x, y;
    lv_indev_t *in = lv_indev_get_next(NULL);
    if (!in || (!lifted && P->touch_get(&x, &y) != 0)) return;
    P->touch_forget();                                   // and the board's memory of it (no fake press, then a tap)
    in->wait_until_release = 0;
    lv_indev_reset(in, NULL);
    // And ours: LVGL read nothing during the move, so read_hook never saw this press end. A finger landing during the
    // release animation was then taken for the old press (not armed): quick successive swipes were lost (weather_amoled
    // fixed the same with a counter in touch_forget, v1.12.1; found aligning the two, October 4)
    track.down = false;
}

/* The finger, read directly (LVGL paused, the display lock held). The CST9217 often stops answering (NACK) instead of
 * reporting a release: 5 failed reads in a row count as a release, and further failures stay "up" until a press. A
 * reported "up" counts once it has lasted UP_HOLD_US: under a quick finger the chip reports brief "ups" with the finger
 * still down. *errs carries the state: start at 0 (finger down). 1 = down at x,y; 0 = up; -1 / -2 = keep the last
 * point (a read error, or an "up" not held yet). */
#define FINGER_UP 5
#define UP_HOLD_US 60000
static int64_t up_since;

static int finger(int *x, int *y, int *errs)
{
    int r = P->touch_get(x, y);
    if (!P->touch_fresh()) return r > 0 ? 1 : *errs >= FINGER_UP ? 0 : up_since ? -2 : -1;   // same reading
    int64_t now = esp_timer_get_time();
    if (r > 0) { up_since = 0; *errs = 0; return 1; }
    if (*errs >= FINGER_UP) return 0;
    if (r < 0 && !up_since) {
        if (++*errs < FINGER_UP) return -1;
        *errs = FINGER_UP - 1;
    }
    if (!up_since) up_since = now;
    if (now - up_since < UP_HOLD_US) return -2;
    up_since = 0;
    *errs = FINGER_UP;
    return 0;
}

// A loop following the finger ends after 20 s as if it had lifted (a chip stuck reporting a press must not keep the
// display lock forever)
#define FINGER_FOLLOW_US 20000000LL

/* ---------- pager drags ---------- */

#define MAX_PAGERS 6
// A press becomes a pager drag after DRAG_PX, by the larger axis (as LVGL picks a scroll direction). Kconfig
// FORGE_SLIDE_DRAG_PX ("espforge LVGL"): 10 as LVGL's own; esp32-s3-colormesh takes 28 (a tap or a hold with a slight
// finger move became a swipe). The fallback: an emulator sdkconfig.h generated before the option (gen_config.py).
#ifdef CONFIG_FORGE_SLIDE_DRAG_PX
#define DRAG_PX CONFIG_FORGE_SLIDE_DRAG_PX
#else
#define DRAG_PX 10
#endif
// A list scroll (no pager along the finger's axis) is decided sooner: lists keep LVGL's 10 px whatever the pagers take
// (at 28 a list would lag the finger by 28 px: scroll_run follows from where it was decided). A finger that may still
// become a drag of a pager across waits for DRAG_PX (read_hook).
#define SCROLL_PX (DRAG_PX < 10 ? DRAG_PX : 10)
// LVGL's own scroll starts this far past ours (L99: deciding after LVGL gave quick flicks to its slow scroll).
// read_hook sees each read before LVGL does, so any margin keeps ours first; 10 keeps LVGL at 20 px with the default.
// Set per press: past DRAG_PX where a pager is on view, else past SCROLL_PX (a list LVGL scrolls itself, a horizontal
// one or on a board without raw_area, keeps 20 px instead of 2 x 28 = 56 on a screen without a pager).
#define LVGL_MARGIN_PX 10
static lv_obj_t *pagers[MAX_PAGERS];
static int npagers;
static struct { bool queued, vertical; lv_obj_t *pager; int x0, y0, x1, y1; } drag;
static struct { bool queued; int dir; bool vertical; void (*change)(void *); void *user; lv_obj_t *to; } chg;
static struct { lv_obj_t *list; int y0, y1; int64_t t0, t1; bool queued; } scroll;   // a list scroll (below)
static lv_obj_t *slide_scroll_target(lv_obj_t *scr, int x, int y);
static bool slide_scroll(lv_obj_t *list, int y0, int64_t t0, int y1);

bool slide_busy(void) { return drag.queued || chg.queued || scroll.queued; }

static slide_drag_hook_t drag_hook;                         // slide_set_drag_hook
static void *drag_hook_user;
void slide_set_drag_hook(slide_drag_hook_t hook, void *user) { drag_hook = hook; drag_hook_user = user; }

void slide_drop_touch(void)
{
    if (!track.down || track.dropped) return;              // no touch, or this one already dropped
    track.armed = false;                                   // no pager drag, no list scroll from it
    track.dropped = true;
    n_dropped++;
    // LVGL lets go of it too: no scroll of a list under the finger at its own limit, no CLICKED when the finger lifts
    // (a long press's release is a CLICKED, only SHORT_CLICKED is left out); the pressed object gets PRESS_LOST then
    lv_indev_t *in = lv_indev_active();
    if (!in) in = lv_indev_get_next(NULL);
    if (in) lv_indev_wait_release(in);
    ESP_LOGI(TAG, "touch dropped: the app took it, ignored until the finger lifts");
}

static uint32_t moved_ms;                                   // lv_tick when the last move ended (slide_tap_ok)
bool slide_tap_ok(void) { return !slide_busy() && (!moved_ms || lv_tick_elaps(moved_ms) >= SLIDE_TAP_GUARD_MS); }
bool slide_screen_busy(void) { return slide_busy() || lv_tick_elaps(touched_ms) < 500; }

static bool ready(void) { return P && shadow && other && shadow_ok; }

static int slot_of(const lv_obj_t *page)                  // the slot holding this page's picture, -1 if none
{
    for (int i = 0; i < NB; i++) if (nb[i].buf && nb[i].page == page) return i;
    return -1;
}

static int pagers_on_view(lv_obj_t *scr, lv_obj_t **out);

// The pages worth keeping now, best first: prev and next of each pager on view (not those hidden by pager_set_count)
#define NWANT (2 * MAX_PAGERS)
static int wanted(lv_obj_t **want)
{
    lv_obj_t *on[MAX_PAGERS];
    int n = pagers_on_view(lv_screen_active(), on), nw = 0;
    for (int k = 0; k < n; k++) {
        int cur = pager_current(on[k]);
        for (int i = 0; i < 2; i++) {
            lv_obj_t *page = pager_shown(on[k], cur + (i ? 1 : -1));
            if (page) want[nw++] = page;
        }
    }
    return nw;
}

static int worth_of(lv_obj_t *page, lv_obj_t *const *want, int nw)
{
    for (int i = 0; i < nw; i++) if (want[i] == page) return i;
    return nw;
}

// PSRAM for another picture: the free size first (cheap), the largest block (a walk of the heap with interrupts off,
// ~2 ms with 32 MB, L201) only when the free size says it might fit. SLIDE_PSRAM_RESERVE_KB stays for the app.
static bool room_for(int pictures)
{
    size_t one = (size_t)W * H * 2;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) <= SLIDE_PSRAM_RESERVE_KB * 1024 + one * pictures) return false;
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > one + 64 * 1024;
}

// A slot for a new picture of worth want_p (slide_slots.c): with a buffer, allocated now if PSRAM has room, else a
// less useful picture's. -1 = none (the caller renders into `other`, or waits)
static int slot_for(bool force, lv_obj_t *const *want, int nw, int want_p)
{
    slide_slot_t v[NB];
    for (int i = 0; i < NB; i++) v[i] = (slide_slot_t){ nb[i].buf != NULL, nb[i].page, nb[i].used };
    int i = slide_slot_pick(v, NB, (const void *const *)want, nw, force, want_p);
    if (i < 0) return -1;
    if (!nb[i].buf) {
        if (room_for(1)) nb[i].buf = lv_draw_buf_create(W, H, LV_COLOR_FORMAT_RGB565, 0);
        if (!nb[i].buf) {
            i = slide_slot_reuse(v, NB, (const void *const *)want, nw, want_p);
            if (i < 0) return -1;
        }
    }
    nb[i].page = NULL;
    nb[i].at = 0;
    return i;
}

void slide_release_unneeded(void)
{
    lv_obj_t *want[NWANT];
    int nw = wanted(want), n = 0;
    for (int i = 0; i < NB; i++) {
        if (!nb[i].buf || (nb[i].page && worth_of(nb[i].page, want, nw) < nw)) continue;
        lv_draw_buf_destroy(nb[i].buf);
        nb[i] = (typeof(nb[i])){ 0 };
        n++;
    }
    if (n) ESP_LOGI(TAG, "%d picture(s) let go (%d KB of PSRAM)", n, n * W * H * 2 / 1024);
}

// The picture of page `target` of `pg`: the kept one, else rendered now (into a slot, or `other` without one)
static const uint8_t *neighbour(lv_obj_t *pg, int target, int *renders)
{
    lv_obj_t *page = pager_shown(pg, target);
    if (!page) return NULL;
    int i = slot_of(page);
    if (i >= 0 && nb[i].at) { nb[i].used = lv_tick_get(); return nb[i].buf->data; }
    if (i < 0) {
        lv_obj_t *want[NWANT];
        int nw = wanted(want);
        i = slot_for(true, want, nw, worth_of(page, want, nw));   // a drag needs it now: a less useful slot if need be
    }
    lv_draw_buf_t *dst = i >= 0 ? nb[i].buf : other;
    lv_obj_update_layout(lv_obj_get_screen(page));
    if (lv_snapshot_take_to_draw_buf(page, LV_COLOR_FORMAT_RGB565, dst) != LV_RESULT_OK) {
        if (i >= 0) nb[i].page = NULL;
        return NULL;
    }
    (*renders)++;
    if (i >= 0) { nb[i].page = page; nb[i].at = nb[i].used = lv_tick_get(); }
    return dst->data;
}

void slide_stale(void)
{
    for (int i = 0; i < NB; i++) nb[i].at = 0;
}

static void drag_run(void *unused)
{
    lv_obj_t *pg = drag.pager;
    const int S = drag.vertical ? H : W, cur = pager_current(pg), n = pager_count(pg);
    int64_t t0 = esp_timer_get_time();
    phase = 11;
    frame_t f = { .cur = shadow->data, .stride = shadow->header.stride, .vertical = drag.vertical, .pager = pg,
                  .page = cur };
    // The neighbour the finger is heading to, rendered before the first frame (a quick flick may be over by then)
    int side = (drag.vertical ? drag.y1 - drag.y0 : drag.x1 - drag.x0) > 0 ? -1 : 1, target = cur + side;
    int renders = 0;
    const uint8_t *pic = neighbour(pg, target, &renders);
    bool have = pic != NULL;
    if (side < 0) f.prev = pic; else f.next = pic;
    (void)n;
    int x = drag.x1, y = drag.y1, raw = 0, frames = 0, samples = 0, errs = 0;
    int64_t t_first = 0, t_prev = esp_timer_get_time(), t_last = t_prev;
    // For the log line: what a real finger does (synthetic drags never hold): reads held by a bus error / a brief
    // "up", the longest run of held reads, the longest gap between two frames, the longest the finger stood still
    int held_err = 0, held_up = 0;
    int64_t hold_since = 0, hold_max = 0, frame_at = 0, gap_max = 0, still_since = 0, still_max = 0;
    int last_raw = 0;
    int pos_prev = 0, pos_last = 0;
    phase = 12;
    for (;;) {
        int nx, ny, r = finger(&nx, &ny, &errs);
        if (r == 0) break;
        if (esp_timer_get_time() - t0 > FINGER_FOLLOW_US) { ESP_LOGW(TAG, "drag: finger down for 20 s, ending it"); break; }
        if (r > 0) { x = nx; y = ny; samples++; }
        raw = drag.vertical ? y - drag.y0 : x - drag.x0;     // > 0: towards prev (it comes in from the left / top)
        // Held (a read error, or an "up" not confirmed yet: up to 60 ms): the page goes on at the finger's last speed
        // instead of standing still (every swipe ended with a ~50 ms freeze, then the snap), at most 80 ms worth
        if (r < 0 && t_last > t_prev) {
            int64_t ahead = esp_timer_get_time() - t_last;
            if (ahead > 80000) ahead = 80000;
            raw = pos_last + (int)((pos_last - pos_prev) * (float)ahead / (t_last - t_prev));
        }
        int s = raw > 0 ? -1 : raw < 0 ? 1 : 0;
        int64_t tnow = esp_timer_get_time();
        if (r < 0) {
            if (r == -1) held_err++; else held_up++;
            if (!hold_since) hold_since = tnow;
            if (tnow - hold_since > hold_max) hold_max = tnow - hold_since;
        } else hold_since = 0;
        if (s && s != side) {                                // the finger turned: the other neighbour
            side = s;
            target = cur + side;
            pic = neighbour(pg, target, &renders);
            have = pic != NULL;
            f.prev = side < 0 ? pic : NULL;
            f.next = side > 0 ? pic : NULL;
        }
        int off;
        if (s && have) off = raw < -S ? -S : raw > S ? S : raw;   // follows the finger
        else {                                                     // no neighbour: resists, at most a fifth
            off = raw / 3;
            if (off > S / 5) off = S / 5;
            if (off < -S / 5) off = -S / 5;
        }
        int64_t now = esp_timer_get_time();
        // Speed from fresh readings only: a repeated point measured a flick as 0 px/ms (weather_amoled v1.12.1)
        if (r > 0 && P->touch_fresh() && now - t_last > 8000) { t_prev = t_last; pos_prev = pos_last; t_last = now; pos_last = raw; }
        if (raw != last_raw || !still_since) { still_since = now; last_raw = raw; }
        else if (now - still_since > still_max) still_max = now - still_since;
        show(&f, off);
        int64_t done = esp_timer_get_time();
        if (frame_at && done - frame_at > gap_max) gap_max = done - frame_at;
        frame_at = done;
        if (!frames++) t_first = done;
    }
    // Released: on to the neighbour past a third of the screen or after a flick towards it, else back
    float vel = t_last > t_prev ? (float)(pos_last - pos_prev) / ((t_last - t_prev) / 1000.0f) : 0;   // px/ms
    int s = f.off > 0 ? -1 : f.off < 0 ? 1 : 0;
    bool go = s && have && s == side &&
              (abs(f.off) > S / 3 || (abs(f.off) > 24 && vel * f.off > 0 && fabsf(vel) > 0.35f));
    if (!samples && have) go = true;                         // lifted before the first frame: a flick that way
    int to = go ? (side < 0 ? S : -S) : 0;
    frames += animate(&f, to, 60 + 220 * abs(to - f.off) / S);
    int64_t t1 = esp_timer_get_time();
    phase = 16;
    if (go) {
        lv_obj_t *arrived = pager_shown(pg, target), *left = pager_shown(pg, cur);
        pager_switch(pg, target);                           // no LVGL animation: the pictures already moved
        // The page left is now a neighbour: its picture is the shadow (what the panel showed when the drag started),
        // in the slot of the page arrived at (no longer a neighbour). After pager_switch: the app's on_settle may have
        // called slide_stale(). The one beyond: rendered when idle.
        int back = slot_of(arrived);
        if (back < 0) back = slot_of(left);
        if (back < 0) {
            lv_obj_t *want[NWANT];
            int nw = wanted(want);
            back = slot_for(false, want, nw, worth_of(left, want, nw));
        }
        if (back >= 0) {
            memcpy(nb[back].buf->data, shadow->data, shadow->header.stride * H);
            nb[back].page = left;
            nb[back].at = nb[back].used = lv_tick_get();
        }
    }
    lv_obj_invalidate(lv_screen_active());                  // LVGL repaints the real thing (and the shadow)
    touch_resync(true);
    touched_ms = lv_tick_get();                              // LVGL saw no reads during the drag: quiet from now
    int64_t ts = t_first ? t_first : t0;
    ESP_LOGI(TAG, "drag: first frame after %lld ms, %d frames in %lld ms (%.0f fps), %s | gap max %lld ms, held reads "
                  "%d err %d up (longest %lld ms), finger still max %lld ms, samples %d, renders %d, %.2f px/ms",
             (ts - t0) / 1000, frames, (t1 - ts) / 1000, frames * 1e6f / (t1 - ts + 1),
             go ? (side < 0 ? "to prev" : "to next") : "back", gap_max / 1000, held_err, held_up, hold_max / 1000,
             still_max / 1000, samples, renders, vel);
    moved_ms = lv_tick_get();
    drag.queued = false;
    phase = 0;
}

// The pagers on view on this screen (pager_on_view: one on a page of another counts while that page is shown)
static int pagers_on_view(lv_obj_t *scr, lv_obj_t **out)
{
    int n = 0;
    for (int i = 0; i < npagers; i++)
        if (lv_obj_get_screen(pagers[i]) == scr && pager_on_view(pagers[i])) out[n++] = pagers[i];
    return n;
}

// The pager a drag along this axis moves: the deepest on view that way (a pager on a page of another is above it)
static lv_obj_t *pager_for(lv_obj_t *scr, bool vertical)
{
    lv_obj_t *on[MAX_PAGERS], *best = NULL;
    int n = pagers_on_view(scr, on), depth = -1;
    for (int i = 0; i < n; i++) {
        if (pager_vertical(on[i]) != vertical) continue;
        int d = 0;
        for (lv_obj_t *o = on[i]; o; o = lv_obj_get_parent(o)) d++;
        if (d > depth) { depth = d; best = on[i]; }
    }
    return best;
}

// Each touch read, before LVGL handles it: a press on a screen with a slide pager on view that moves DRAG_PX becomes a
// drag of the pager on view along that axis (with a pager on a page of another, each axis has its own); with none that
// way, a list under the finger after SCROLL_PX (drawn as pictures), else it stays LVGL's (its scrolling, a gesture)
static void read_hook(lv_indev_t *in, lv_indev_data_t *data)
{
    if (data->state != LV_INDEV_STATE_PRESSED) { track.down = false; return; }
    touched_ms = lv_tick_get();
    track.p = data->point;
    if (!track.down) {                                     // a new press: may become a drag
        lv_obj_t *on[MAX_PAGERS];
        int n = pagers_on_view(lv_screen_active(), on);
        track.down = true;
        track.dropped = false;
        track.armed = !slide_busy() && (n > 0 || (P && P->raw_area));
        track.p0 = data->point;
        track.t0 = esp_timer_get_time();
        lv_indev_set_scroll_limit(in, (n ? DRAG_PX : SCROLL_PX) + LVGL_MARGIN_PX);   // LVGL's after ours
        return;
    }
    if (!track.armed) return;
    int dx = abs(data->point.x - track.p0.x), dy = abs(data->point.y - track.p0.y), moved = dx > dy ? dx : dy;
    if (moved < SCROLL_PX) return;
    bool vertical = dy > dx;
    lv_obj_t *scr = lv_screen_active();
    track.pager = pager_for(scr, vertical);
    if (!track.pager) {                                    // no pager that way: a list under the finger, as pictures
        if (vertical && P->raw_area && ready() && !slide_busy()) {
            lv_obj_t *list = slide_scroll_target(scr, track.p0.x, track.p0.y);
            if (list && slide_scroll(list, track.p0.y, track.t0, data->point.y)) {
                track.armed = false;
                n_scrolls++;
                lv_indev_wait_release(in);
                return;
            }
        }
        if (moved < DRAG_PX && pager_for(scr, !vertical)) return;   // may still turn into a drag across
        track.armed = false;
        return;                                            // else LVGL's (a gesture)
    }
    if (moved < DRAG_PX) return;                           // a pager that way: decided at DRAG_PX
    track.armed = false;
    if (!ready() || slide_busy()) return;
    drag = (typeof(drag)){ true, vertical, track.pager, track.p0.x, track.p0.y, data->point.x, data->point.y };
    lv_async_call(drag_run, NULL);                         // at the top of the LVGL task (stack for the render)
    lv_indev_wait_release(in);                             // the drag owns this touch: LVGL ignores it from now on
    n_drags++;
    // LVGL sends the pressed object no PRESS_LOST (wait_release, then touch_resync's reset after the move): the app
    // learns here that its press became a drag (esp32-s3-colormesh's "hold", which stayed on for the whole swipe)
    if (drag_hook) { n_hooked++; drag_hook(track.pager, drag_hook_user); }
}

void slide_pager(lv_obj_t *pager)
{
    if (npagers < MAX_PAGERS) pagers[npagers++] = pager;
    if (P) pager_freeze(pager);                            // drags are ours; without the panel LVGL keeps them
}

/* ---------- list scrolls drawn as pictures ----------
 * A vertical drag on a scrollable list with no pager that way (the Settings screen) is drawn from the shadow: each frame
 * moves the list's rows within the picture, renders the rows coming in (LVGL, clipped to a strip) and sends only the
 * list's rectangle (P->raw_area). LVGL's own scrolling redrew the whole list every frame (~25 fps). From weather_amoled's
 * slide.c (v1.14), without its sideways hand-off. */

// How far a label's glyphs may reach past its box: LVGL skips a label whose box misses the rows drawn, but syllabics'
// tops rise ROW_UP rows above the box at 28 px and descenders go ROW_DOWN below (Montserrat alone: 4 and 1). Those rows
// are rendered too, and the outer margin rows put back as they were.
#define ROW_UP 9
#define ROW_DOWN 3
#define ROW_MARGIN (ROW_UP + ROW_DOWN)
static uint8_t *keep;                       // ROW_MARGIN rows kept across a strip render (PSRAM, slide_init)

// Rows y0..y1 of a screen into dst (whose first row is `top`): lv_snapshot's path clipped to those rows, so a strip is
// rendered at a time (a whole screen blocks LVGL for 60-180 ms)
static bool render_rows_reach(lv_obj_t *scr, lv_draw_buf_t *dst, int top, int y0, int y1, int up, int down)
{
    lv_obj_update_layout(scr);
    int last = top + (int)dst->header.h - 1;
    int g0 = y0 - up, g1 = y1 + down, nk = 0;
    y0 = LV_MAX(LV_MAX(y0 - up - down, top), 0);
    y1 = LV_MIN(LV_MIN(y1 + up + down, last), H - 1);
    uint32_t st = dst->header.stride;
    if (keep) for (int y = y0; y <= y1; y++) if (y < g0 || y > g1) memcpy(keep + (nk++) * W * 2, dst->data + (y - top) * st, W * 2);
    lv_area_t rows = {0, y0, W - 1, y1}, in_buf = {0, y0 - top, W - 1, y1 - top};
    lv_draw_buf_clear(dst, &in_buf);
    lv_layer_t layer;
    lv_memzero(&layer, sizeof(layer));
    layer.draw_buf = dst;
    layer.buf_area = (lv_area_t){0, top, W - 1, top + dst->header.h - 1};
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = rows;
    layer.phy_clip_area = rows;
    lv_display_t *d = lv_obj_get_display(scr), *old = lv_refr_get_disp_refreshing();
    lv_layer_t *old_head = d->layer_head;
    d->layer_head = &layer;
    lv_refr_set_disp_refreshing(d);
    lv_obj_redraw(&layer, scr);
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }
    d->layer_head = old_head;
    lv_refr_set_disp_refreshing(old);
    nk = 0;
    if (keep) for (int y = y0; y <= y1; y++) if (y < g0 || y > g1) memcpy(dst->data + (y - top) * st, keep + (nk++) * W * 2, W * 2);
    return true;
}

// The scrollable list under a point, if it has room to scroll (else LVGL's own scrolling has nothing to do either)
static lv_obj_t *slide_scroll_target(lv_obj_t *scr, int x, int y)
{
    lv_point_t p = {x, y};
    for (lv_obj_t *o = lv_indev_search_obj(scr, &p); o && o != scr; o = lv_obj_get_parent(o)) {
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) || !(lv_obj_get_scroll_dir(o) & LV_DIR_VER)) continue;
        if (lv_obj_get_scroll_top(o) > 0 || lv_obj_get_scroll_bottom(o) > 0) return o;
    }
    return NULL;
}

static void scroll_run(void *unused);

static bool slide_scroll(lv_obj_t *list, int y0, int64_t t0, int y1)
{
    if (slide_busy() || !shadow_ok) return false;          // no exact picture of the screen: LVGL scrolls
    scroll = (typeof(scroll)){ list, y0, y1, t0, esp_timer_get_time(), true };
    lv_async_call(scroll_run, NULL);
    return true;
}

static void scroll_fill(int x0, int w, int y0, int n, void *dst, void *user)
{
    const lv_draw_buf_t *p = user;
    uint8_t *d = dst;
    for (int y = y0; y < y0 + n; y++, d += w * 2) copy_swap(d, p->data + y * p->header.stride + x0 * 2, w);
}

typedef struct {
    lv_obj_t *scr, *list;
    lv_draw_buf_t *pic;                  // the screen shown (the shadow)
    lv_draw_buf_t *strip;                // the rows coming into view, rendered before the frame is sent
    int d, in0, in1;                     // this frame: rows move by d (> 0: up); rows in0..in1 come from the strip
    int last;                            // the last move's way (> 0: up), for scroll_settle
    lv_area_t r;                         // the list on screen: the rows that move
    int shown;                           // the scroll position the picture shows
    int frames;
    int64_t us_move, us_render, us_send, us_settle;
} scroller_t;

// One pass per frame: each row of the list area is moved within the picture (or taken from the strip) and sent. Moving
// rows down in place needs the bottom rows first: then the bands go bottom up and each band's rows too. From the strip:
// the rows coming in, and ROW_UP above them and ROW_DOWN below (drawn while the next label was still past the list's
// edge, so without its glyphs' tops; a slow drag left 6-7 rows off every time in weather_amoled's Inuktitut).
static void scroll_move_fill(int x0, int w, int y0, int n, void *dst, void *user)
{
    scroller_t *s = user;
    const lv_area_t *r = &s->r;
    uint32_t st = s->pic->header.stride, lw = lv_area_get_width(r) * 2;
    uint8_t *pic = s->pic->data;
    int s0 = s->in0 - ROW_UP, s1 = s->in1 + ROW_DOWN;     // rows taken from the strip
    for (int i = 0; i < n; i++) {
        int y = s->d < 0 ? y0 + n - 1 - i : y0 + i;
        if (y >= r->y1 && y <= r->y2) {
            const uint8_t *src = y >= s0 && y <= s1 ? s->strip->data + (y - s->in0 + ROW_MARGIN) * s->strip->header.stride
                                                    : pic + (y + s->d) * st;
            memcpy(pic + y * st + r->x1 * 2, src + r->x1 * 2, lw);
        }
        copy_swap((uint8_t *)dst + (y - y0) * w * 2, pic + y * st + x0 * 2, w);
    }
}

// Scroll to position `want`: LVGL's state, the picture (moved rows + rendered new ones), the panel
static void scroll_step(scroller_t *s, int want)
{
    int d = want - s->shown, h = lv_area_get_height(&s->r);
    if (!d) return;
    s->last = d;
    // Raw: lv_obj_scroll_to_y() stops at the ends (pulled past them, the picture moved while the list didn't), and
    // lv_obj_scroll_by()'s SCROLL events bubbled up and laid the whole screen out again on every frame
    lv_obj_scroll_by_raw(s->list, 0, lv_obj_get_scroll_y(s->list) - want);
    const lv_area_t *r = &s->r;
    int64_t t0 = esp_timer_get_time(), t1 = t0, t2;
    if (abs(d) < h && s->strip && abs(d) <= (int)s->strip->header.h - 2 * ROW_MARGIN) {
        s->d = d;                                        // the rows coming in (at the bottom when the content goes up)
        s->in0 = d > 0 ? r->y2 - d + 1 : r->y1;
        s->in1 = d > 0 ? r->y2 : r->y1 - d - 1;
        render_rows_reach(s->scr, s->strip, s->in0 - ROW_MARGIN, s->in0, s->in1, ROW_UP, ROW_DOWN);
        t2 = esp_timer_get_time();
        P->raw_area(r->x1, r->y1, r->x2, r->y2, d < 0, scroll_move_fill, s);
    } else {                                             // a jump (or no strip): the whole list, then send
        if (abs(d) >= h) render_rows_reach(s->scr, s->pic, 0, r->y1, r->y2, ROW_UP, ROW_DOWN);
        else {
            uint8_t *base = s->pic->data + r->x1 * 2;
            uint32_t st = s->pic->header.stride, w = lv_area_get_width(r) * 2;
            if (d > 0) for (int y = r->y1; y <= r->y2 - d; y++) memcpy(base + y * st, base + (y + d) * st, w);
            else for (int y = r->y2; y >= r->y1 - d; y--) memcpy(base + y * st, base + (y + d) * st, w);
            t1 = esp_timer_get_time();
            render_rows_reach(s->scr, s->pic, 0, d > 0 ? r->y2 - d + 1 : r->y1, d > 0 ? r->y2 : r->y1 - d - 1, ROW_UP, ROW_DOWN);
        }
        t2 = esp_timer_get_time();
        P->raw_area(r->x1, r->y1, r->x2, r->y2, false, scroll_fill, s->pic);
    }
    s->us_move += t1 - t0;
    s->us_render += t2 - t1;
    s->us_send += esp_timer_get_time() - t2;
    s->shown = want;
    s->frames++;
}

// After the last frame, the edge the list last moved away from: a label whose box has just gone past it was skipped by
// LVGL, but the rows moved within the picture still hold what its glyphs reached past the box. Rendered again, once.
static void scroll_settle(scroller_t *s)
{
    if (!s->last) return;
    int64_t t0 = esp_timer_get_time();
    const lv_area_t *r = &s->r;
    int y0 = s->last < 0 ? r->y2 - ROW_UP + 1 : r->y1, y1 = s->last < 0 ? r->y2 : r->y1 + ROW_DOWN - 1;
    render_rows_reach(s->scr, s->pic, 0, y0, y1, ROW_UP, ROW_DOWN);
    P->raw_area(0, y0 - ROW_UP, W - 1, y1 + ROW_DOWN, false, scroll_fill, s->pic);
    s->us_settle = esp_timer_get_time() - t0;
}

static void scroll_run(void *unused)
{
    scroller_t s = { .scr = lv_screen_active(), .list = scroll.list, .pic = shadow };
    lv_display_t *disp = lv_display_get_default();
    phase = 31;
    if (!shadow_ok || !lv_obj_is_valid(s.list)) { scroll.queued = false; phase = 0; return; }
    s.strip = lv_draw_buf_create(W, 96 + 2 * ROW_MARGIN, LV_COLOR_FORMAT_RGB565, 0);   // 112 KB; NULL: slower path
    int64_t t0 = esp_timer_get_time();
    // LVGL lets go of the touch (the row pressed under the finger, its own scroll if it had begun) and draws that now:
    // to the panel and, through flushed(), into the shadow
    lv_indev_t *in = lv_indev_get_next(NULL);
    lv_indev_reset(in, NULL);
    lv_refr_now(disp);
    uint32_t inv0 = disp->inv_p;
    lv_obj_get_coords(s.list, &s.r);
    lv_area_t screen = {0, 0, W - 1, H - 1};
    lv_area_intersect(&s.r, &s.r, &screen);
    const int LH = lv_area_get_height(&s.r);
    s.shown = lv_obj_get_scroll_y(s.list);
    const int lo = s.shown - lv_obj_get_scroll_top(s.list), hi = s.shown + lv_obj_get_scroll_bottom(s.list);
    const int over = LH / 5;                             // how far past an end it can be pulled
    phase = 32;

    int ref_y = scroll.y1, ref_pos = s.shown, pos = s.shown, fy = scroll.y1, errs = 0, moved = 0;
    float fpos = pos, v = 0;
    struct { int64_t t; int p; } smp[8];                 // finger samples (time, position) for the flick speed:
    smp[0].t = scroll.t0;                                // the press and the SCROLL_PX that made it a scroll come first
    smp[0].p = ref_pos + (scroll.y1 - scroll.y0);        // (a flick can be over by now)
    smp[1].t = scroll.t1;
    smp[1].p = ref_pos;
    int ns = 2;
    bool down = true;
    // A touch while coasting stops the list; it moves again only once the finger has moved SCROLL_PX (a new touch is
    // decided). maybe_up: coasting on an "up" not confirmed yet.
    bool deciding = false, maybe_up = false;
    int lifts = 0, touches = 0, read_errs = 0;
    int64_t last_read = 0, gap_max = 0;
    int tx0 = 0, ty0 = 0;
    int64_t t = esp_timer_get_time(), touched_at = t;
    for (;;) {
        int x, y, r = finger(&x, &y, &errs);
        int64_t now = esp_timer_get_time();
        if (down) {
            if (r == -1) read_errs++;
            if (last_read && now - last_read > gap_max) gap_max = now - last_read;
            last_read = now;
        } else last_read = 0;
        if (down && deciding && r != 0) {
            if (r < 0 || (abs(x - tx0) < SCROLL_PX && abs(y - ty0) < SCROLL_PX)) { vTaskDelay(1); continue; }
            deciding = false;
            ref_y = ty0;                                 // the list follows from where the touch went down
            fy = y;                                      // (catching up the SCROLL_PX it took to decide)
            ref_pos = pos;
            smp[0].t = touched_at;
            smp[0].p = ref_pos;
            smp[1].t = now;
            smp[1].p = ref_pos + (ty0 - y);
            ns = 2;
        }
        if (down) {
            bool stuck = now - touched_at > FINGER_FOLLOW_US;
            if (stuck) ESP_LOGW(TAG, "scroll: finger down for 20 s, ending it");
            if (r == 0 || r == -2 || stuck) {            // lifted (or 5 silent reads), or maybe: coast, follow again
                maybe_up = r == -2;                      // if the finger is back
                if (!maybe_up) lifts++;
                down = false;
                deciding = false;
                v = 0;                                   // speed over the last ~80 ms (0 if the finger stopped)
                if (ns) {
                    int nk = (ns - 1) % 8;
                    for (int i = ns - 2; i >= 0 && i >= ns - 8; i--) {
                        int k = i % 8;
                        int64_t span = smp[nk].t - smp[k].t;
                        if (span > 80000) break;
                        if (span >= 10000) v = (float)(smp[nk].p - smp[k].p) / (span / 1000.0f);
                    }
                    if (now - smp[nk].t > 80000 + UP_HOLD_US) v = 0;   // (the lift is known UP_HOLD_US late)
                }
                fpos = pos;
                t = now;
                if (stuck) break;
                continue;
            }
            if (r > 0) fy = y;                           // (a read error: keep the last point)
            int raw = ref_pos + (ref_y - fy);            // the content follows the finger
            if (r > 0 && P->touch_fresh() && (!ns || now - smp[(ns - 1) % 8].t >= 8000)) { smp[ns % 8].t = now; smp[ns % 8].p = raw; ns++; }
            int want = raw < lo ? lo - (lo - raw) / 3 : raw > hi ? hi + (raw - hi) / 3 : raw;   // resists past an end
            if (want < lo - over) want = lo - over;
            if (want > hi + over) want = hi + over;
            if (want == s.shown) { vTaskDelay(1); continue; }
            moved += abs(want - pos);
            pos = want;
            scroll_step(&s, pos);
            continue;
        }
        if (maybe_up && r >= 0) {                        // the "up" settled: a real lift, or the same finger back
            maybe_up = false;
            if (r == 0) lifts++;
            else if (r > 0) {                            // it never lifted (a brief "up"): follow it again from here
                down = true;
                ref_y = fy = y;
                ref_pos = pos;
                smp[0].t = now;
                smp[0].p = pos;
                ns = 1;
                continue;
            }
        }
        if (r > 0) {                                     // touched while moving: stop, and see where the finger goes
            touches++;
            down = deciding = true;
            touched_at = now;
            tx0 = x;
            ty0 = y;
            ref_y = fy = y;
            ref_pos = pos;
            ns = 0;
            continue;
        }
        float dt = (now - t) / 1000.0f;
        t = now;
        bool past = pos < lo || pos > hi;
        if (past) v *= expf(-dt / 30.0f);                // past an end: brake hard...
        else v *= expf(-dt / 300.0f);                    // a flick goes on ~v x 300 ms, as LVGL's
        if (past ? fabsf(v) < 0.1f : fabsf(v) < 0.03f) {
            if (!past && maybe_up) { vTaskDelay(1); continue; }   // stopped, but the finger may still be there
            if (!past) break;
            int from = pos, to = pos < lo ? lo : hi;     // ...then spring back to the end (ease out, 220 ms)
            int64_t start = esp_timer_get_time();
            bool touched = false;
            for (;;) {
                float k = (esp_timer_get_time() - start) / 220000.0f;
                if (k > 1) k = 1;
                float ease = 1 - (1 - k) * (1 - k) * (1 - k);
                pos = from + (int)((to - from) * ease);
                scroll_step(&s, pos);
                if (k >= 1) break;
                if (finger(&x, &y, &errs) > 0) { touched = true; touches++; break; }
            }
            if (!touched && maybe_up) { fpos = pos; v = 0; continue; }   // (undecided "up": wait at the end)
            if (!touched) break;
            down = deciding = true;
            touched_at = esp_timer_get_time();
            tx0 = x;
            ty0 = y;
            ref_y = fy = y;
            ref_pos = pos;
            fpos = pos;
            v = 0;
            ns = 0;
            continue;
        }
        fpos += v * dt;
        if (fpos < lo - over) { fpos = lo - over; v = 0; }
        if (fpos > hi + over) { fpos = hi + over; v = 0; }
        if ((int)fpos == pos) { vTaskDelay(1); continue; }
        moved += abs((int)fpos - pos);
        pos = (int)fpos;
        scroll_step(&s, pos);
    }
    scroll_settle(&s);
    disp->inv_p = inv0;                                  // LVGL's redraw requests for the list: already on the panel
    if (s.strip) lv_draw_buf_destroy(s.strip);
    touch_resync(true);
    touched_ms = lv_tick_get();
    int64_t t1 = esp_timer_get_time();
    int f = s.frames ? s.frames : 1;
    ESP_LOGI(TAG, "scroll: %d frames in %lld ms, moved %d px, now at %d of %d..%d; per frame: move %.1f, render %.1f, "
                  "send %.1f ms, then the edge's rows %.1f ms; finger: %d lifts, %d touches again, %d read errors, "
                  "longest gap %lld ms", s.frames, (t1 - t0) / 1000, moved, pos, lo, hi, s.us_move / 1000.0f / f,
             s.us_render / 1000.0f / f, s.us_send / 1000.0f / f, s.us_settle / 1000.0f, lifts, touches, read_errs,
             gap_max / 1000);
    moved_ms = lv_tick_get();
    scroll.queued = false;
    phase = 0;
}

/* ---------- in-place changes and screen loads ---------- */

static void change_run(void *unused)
{
    phase = 21;
    int64_t t0 = esp_timer_get_time();
    if (chg.change) chg.change(chg.user);                  // the new state (the shadow still holds the old one)
    lv_obj_t *scr = chg.to ? chg.to : lv_screen_active();
    bool ok = render(scr);
    int frames = 0;
    int64_t t1 = esp_timer_get_time();
    if (ok) {
        frame_t f = { .cur = shadow->data, .stride = shadow->header.stride, .vertical = chg.vertical };
        if (chg.dir > 0) f.next = other->data; else f.prev = other->data;
        phase = 22;
        frames = animate(&f, -chg.dir * (chg.vertical ? H : W), 280);
    }
    int64_t t2 = esp_timer_get_time();
    if (chg.to) lv_screen_load(chg.to);
    lv_obj_invalidate(scr);
    touch_resync(false);
    if (ok) ESP_LOGI(TAG, "change: picture %lld ms, %d frames in %lld ms (%.0f fps)", (t1 - t0) / 1000, frames,
                     (t2 - t1) / 1000, frames * 1e6f / (t2 - t1 + 1));
    else ESP_LOGW(TAG, "change: no picture, no animation");
    moved_ms = lv_tick_get();
    chg.queued = false;
    phase = 0;
}

static void queue_change(int dir, bool vertical, void (*change)(void *), void *user, lv_obj_t *to)
{
    if (!ready() || slide_busy()) {                        // no slide possible: the change itself, at once
        if (change) change(user);
        if (to) lv_screen_load_anim(to, dir > 0 ? (vertical ? LV_SCR_LOAD_ANIM_MOVE_TOP : LV_SCR_LOAD_ANIM_MOVE_LEFT) :
                                    (vertical ? LV_SCR_LOAD_ANIM_MOVE_BOTTOM : LV_SCR_LOAD_ANIM_MOVE_RIGHT), 250, 0, false);
        return;
    }
    chg = (typeof(chg)){ true, dir, vertical, change, user, to };
    lv_async_call(change_run, NULL);
}

void slide_change(int dir, bool vertical, void (*change)(void *user), void *user)
{
    queue_change(dir, vertical, change, user, NULL);
}

void slide_to(lv_obj_t *scr, int dir, bool vertical)
{
    if (scr != lv_screen_active()) queue_change(dir, vertical, NULL, NULL, scr);
}

// Keeps the neighbours of the pages shown ready (see nb): one picture per tick, only while nobody touches the screen
static void idle_tick(lv_timer_t *t)
{
    if (!ready() || slide_busy() || lv_tick_elaps(touched_ms) < IDLE_MS) return;
    lv_obj_t *want[NWANT];
    int nw = wanted(want);
    for (int w = 0; w < nw; w++) {
        int i = slot_of(want[w]);
        if (i >= 0 && nb[i].at && lv_tick_elaps(nb[i].at) < STALE_MS) continue;
        if (i < 0) i = slot_for(false, want, nw, w);       // a slot worth less than this page, with a buffer
        if (i < 0) return;
        lv_obj_update_layout(lv_obj_get_screen(want[w]));
        bool ok = lv_snapshot_take_to_draw_buf(want[w], LV_COLOR_FORMAT_RGB565, nb[i].buf) == LV_RESULT_OK;
        nb[i].page = ok ? want[w] : NULL;
        nb[i].at = nb[i].used = lv_tick_get();
        return;                                             // one per tick: LVGL stays responsive
    }
}

static void where_slide(char *out, size_t n) { snprintf(out, n, " slide_phase=%d", phase); }

static int ms_arg(const char *s) { int v = atoi(s); return v < 0 ? 0 : v > 10000 ? 10000 : v; }   // 0..10000 ms

/* "slide": the thresholds and the counters since boot (no lock: volatile counters; lvgl_px is the last press's).
 * "slide hold X Y MS X2 Y2 [MOVE_MS [END_MS]]": a finger held MS at X Y, then moved to X2 Y2 over MOVE_MS (default
 * 400), kept there END_MS (default 0) and lifted. forge_lvgl's drag moves from the start, its press never moves: this
 * is the hold that becomes a long press and then moves on (harness long_press_drops_touch), or a short move that
 * stops before the lift (drag_threshold: no flick). */
static void cmd_slide(int argc, char **argv)
{
    if (argc == 1) {
        lv_indev_t *in = lv_indev_get_next(NULL);
        ESP_LOGI("test", "slide drag_px=%d scroll_px=%d lvgl_px=%d drags=%lu scrolls=%lu hooked=%lu dropped=%lu",
                 DRAG_PX, SCROLL_PX, in ? (int)in->scroll_limit : -1, (unsigned long)n_drags,
                 (unsigned long)n_scrolls, (unsigned long)n_hooked, (unsigned long)n_dropped);
        return;
    }
    if (strcmp(argv[1], "hold") || argc < 7) {
        ESP_LOGW("test", "error slide [hold X Y MS X2 Y2 [MOVE_MS [END_MS]]]");
        return;
    }
    int x0 = atoi(argv[2]), y0 = atoi(argv[3]), x1 = atoi(argv[5]), y1 = atoi(argv[6]);
    int move = argc > 7 ? ms_arg(argv[7]) : 400, steps = move / 16 < 2 ? 2 : move / 16;
    finger_inject(true, x0, y0);
    vTaskDelay(pdMS_TO_TICKS(ms_arg(argv[4])));
    for (int i = 1; i <= steps; i++) {                     // steps of ~16 ms, as forge_lvgl's drag
        finger_inject(true, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
    if (argc > 8) vTaskDelay(pdMS_TO_TICKS(ms_arg(argv[8])));
    finger_inject(false, x1, y1);
    vTaskDelay(pdMS_TO_TICKS(100));                        // LVGL must read the release before the real controller
    finger_inject_end();
    ESP_LOGI("test", "ok slide hold");
}

void slide_init(void)
{
    extern const forge_panel_t *forge_lvgl_panel(void);
    // The console command first: it answers (counters at 0) on a board without the panel hooks too
    testcon_register("slide", "slide [hold X Y MS X2 Y2 [MOVE_MS [END_MS]]]", cmd_slide);
    P = forge_lvgl_panel();
    W = lv_display_get_horizontal_resolution(NULL);
    H = lv_display_get_vertical_resolution(NULL);
    // Two full-screen pictures in PSRAM (LVGL's heap): 434 KB each at 466x466
    shadow = lv_draw_buf_create(W, H, LV_COLOR_FORMAT_RGB565, 0);
    other = lv_draw_buf_create(W, H, LV_COLOR_FORMAT_RGB565, 0);
    if (!P || !shadow || !other) { ESP_LOGW(TAG, "no pictures: LVGL's own animations"); return; }
    lv_draw_buf_clear(shadow, NULL);
    P->set_flush_hook(flushed);
    P->set_read_hook(read_hook);
    lv_display_add_event_cb(lv_display_get_default(), rendered, LV_EVENT_RENDER_READY, NULL);
    // LVGL's own scroll starts after 10 px too: a read landing between its decision and ours went to LVGL (a list
    // scrolled at ~20 fps, the touch read only between its frames). Ours decides first, LVGL's LVGL_MARGIN_PX later
    // (LESSONS L99); read_hook sets it again at each press (DRAG_PX or SCROLL_PX, by the pagers on view).
    lv_indev_t *in = lv_indev_get_next(NULL);
    if (in) lv_indev_set_scroll_limit(in, DRAG_PX + LVGL_MARGIN_PX);
    ESP_LOGI(TAG, "a pager drag after %d px, a list scroll after %d px (LVGL's own: %d px more)", DRAG_PX, SCROLL_PX,
             LVGL_MARGIN_PX);
    lv_obj_invalidate(lv_screen_active());                 // a whole frame for the shadow
    for (int i = 0; i < npagers; i++) pager_freeze(pagers[i]);
    ESP_LOGI(TAG, "pictures: shadow and other (%d KB each); up to %d neighbours allocated when needed, %d KB kept for "
                  "the app", W * H * 2 / 1024, NB, SLIDE_PSRAM_RESERVE_KB);
    lv_timer_create(idle_tick, 100, NULL);
    keep = heap_caps_malloc(ROW_MARGIN * W * 2, MALLOC_CAP_SPIRAM);   // list scrolls: the margin rows kept
    testcon_add_where(where_slide);
}
