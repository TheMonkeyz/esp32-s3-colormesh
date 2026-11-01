// forge_lvgl pager.c: pager_set_count, pager_shown, pager_set_order (hidden pages out of reach, order kept)
#include <string.h>
#include "check.h"
#include "pager.h"

// ---- the LVGL fake (lvfake/lvgl.h) ----
#define W 466
#define H 466
#define MAXC 16
struct lv_obj_t {
    lv_obj_t *parent, *child[MAXC];
    int nchild;
    int32_t x, y, w, h, sx, sy;
    uint32_t flags;
    void *user;
    struct { lv_event_cb_t cb; lv_event_code_t code; } ev[4];
    int nev;
};
struct lv_event_t { lv_obj_t *target, *current; lv_event_code_t code; };
static lv_obj_t objs[96];
static int nobj;

int32_t lv_display_get_horizontal_resolution(void *d) { (void)d; return W; }
int32_t lv_display_get_vertical_resolution(void *d) { (void)d; return H; }
lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    lv_obj_t *o = &objs[nobj++];
    memset(o, 0, sizeof(*o));
    o->parent = parent;
    if (parent) parent->child[parent->nchild++] = o;
    return o;
}
lv_obj_t *lv_obj_get_parent(const lv_obj_t *o) { return o->parent; }
void lv_obj_remove_style_all(lv_obj_t *o) { (void)o; }
void lv_obj_set_size(lv_obj_t *o, int32_t w, int32_t h) { o->w = w; o->h = h; }
void lv_obj_set_pos(lv_obj_t *o, int32_t x, int32_t y) { o->x = x; o->y = y; }
void lv_obj_set_user_data(lv_obj_t *o, void *u) { o->user = u; }
void *lv_obj_get_user_data(lv_obj_t *o) { return o->user; }
void lv_obj_add_flag(lv_obj_t *o, uint32_t f) { o->flags |= f; }
void lv_obj_remove_flag(lv_obj_t *o, uint32_t f) { o->flags &= ~f; }
bool lv_obj_has_flag(const lv_obj_t *o, uint32_t f) { return (o->flags & f) == f; }
void lv_obj_set_scroll_dir(lv_obj_t *o, int d) { (void)o; (void)d; }
void lv_obj_set_scroll_snap_x(lv_obj_t *o, int s) { (void)o; (void)s; }
void lv_obj_set_scroll_snap_y(lv_obj_t *o, int s) { (void)o; (void)s; }
void lv_obj_set_scrollbar_mode(lv_obj_t *o, int m) { (void)o; (void)m; }
void lv_obj_add_event_cb(lv_obj_t *o, lv_event_cb_t cb, lv_event_code_t code, void *u)
{
    (void)u;
    o->ev[o->nev].cb = cb;
    o->ev[o->nev++].code = code;
}
void lv_obj_update_layout(lv_obj_t *o) { (void)o; }
static void send(lv_obj_t *o, lv_event_code_t code)
{
    lv_event_t e = { o, o, code };
    for (int i = 0; i < o->nev; i++) if (o->ev[i].code == code) o->ev[i].cb(&e);
}
// LVGL's scroll range: up to the far edge of the last shown child
static int32_t range(const lv_obj_t *o, bool vertical)
{
    int32_t end = 0;
    for (int i = 0; i < o->nchild; i++) {
        const lv_obj_t *c = o->child[i];
        if (c->flags & LV_OBJ_FLAG_HIDDEN) continue;
        int32_t e = vertical ? c->y + c->h : c->x + c->w;
        if (e > end) end = e;
    }
    int32_t r = end - (vertical ? o->h : o->w);
    return r < 0 ? 0 : r;
}
static int32_t clamp(int32_t v, int32_t hi) { return v < 0 ? 0 : v > hi ? hi : v; }
void lv_obj_scroll_to_x(lv_obj_t *o, int32_t x, lv_anim_enable_t a)
{
    (void)a;
    o->sx = clamp(x, range(o, false));
    send(o, LV_EVENT_SCROLL);
}
void lv_obj_scroll_to_y(lv_obj_t *o, int32_t y, lv_anim_enable_t a)
{
    (void)a;
    o->sy = clamp(y, range(o, true));
    send(o, LV_EVENT_SCROLL);
}
// LVGL keeps the scroll position in range when the content shrinks (a page hidden): the fake does it on read
int32_t lv_obj_get_scroll_x(const lv_obj_t *o) { return clamp(o->sx, range(o, false)); }
int32_t lv_obj_get_scroll_y(const lv_obj_t *o) { return clamp(o->sy, range(o, true)); }
int32_t lv_obj_get_x(const lv_obj_t *o) { return o->x; }
int32_t lv_obj_get_y(const lv_obj_t *o) { return o->y; }
lv_obj_t *lv_event_get_target(lv_event_t *e) { return e->target; }
lv_obj_t *lv_event_get_current_target(lv_event_t *e) { return e->current; }
lv_event_code_t lv_event_get_code(lv_event_t *e) { return e->code; }

static bool hidden(lv_obj_t *o) { return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); }

int main(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *pg = pager_create(scr, false, 5, NULL, NULL, NULL);
    lv_obj_t *p[5];
    for (int i = 0; i < 5; i++) p[i] = pager_page(pg, i);
    CHECK(pager_count(pg) == 5, "%d", pager_count(pg));

    // set_count: the pages past n hidden, out of the scroll range and of the drags' neighbours
    pager_go(pg, 4, false);
    pager_set_count(pg, 3);
    CHECK(pager_count(pg) == 3, "%d", pager_count(pg));
    CHECK(!hidden(p[2]) && hidden(p[3]) && hidden(p[4]), "flags");
    CHECK(pager_current(pg) == 2, "current %d after the page shown went away", pager_current(pg));
    CHECK(pager_page(pg, 4) == p[4], "pager_page still gives a hidden page");
    CHECK(pager_shown(pg, 2) == p[2] && !pager_shown(pg, 3) && !pager_shown(pg, -1), "pager_shown");
    pager_go(pg, 4, false);
    CHECK(pager_current(pg) == 2, "a hidden page can't be scrolled to: %d", pager_current(pg));
    pager_set_count(pg, 0);
    CHECK(pager_count(pg) == 1, "at least one page: %d", pager_count(pg));
    pager_set_count(pg, 99);
    CHECK(pager_count(pg) == 5 && !hidden(p[4]), "at most the pages created: %d", pager_count(pg));

    // set_order: the pages listed first, in that order; the others hidden after them; positions follow
    lv_obj_t *want[] = { p[3], p[1], p[4] };
    pager_set_order(pg, want, 3);
    CHECK(pager_count(pg) == 3, "%d", pager_count(pg));
    CHECK(pager_page(pg, 0) == p[3] && pager_page(pg, 1) == p[1] && pager_page(pg, 2) == p[4], "order");
    CHECK(pager_index(pg, p[4]) == 2 && pager_index(pg, p[0]) >= 3, "index %d %d", pager_index(pg, p[4]),
          pager_index(pg, p[0]));
    CHECK(lv_obj_get_x(p[3]) == 0 && lv_obj_get_x(p[1]) == W && lv_obj_get_x(p[4]) == 2 * W, "positions");
    CHECK(hidden(p[0]) && hidden(p[2]) && !hidden(p[3]), "flags after order");
    pager_go(pg, 2, false);
    CHECK(pager_shown(pg, pager_current(pg)) == p[4], "the last page shown is the one asked last");

    // a page listed twice, or not of this pager, is skipped (the copy in esp32-s3-rtcquebec overran its array: ASan)
    lv_obj_t *other = lv_obj_create(scr);
    lv_obj_t *dups[] = { p[2], p[2], other, p[2], p[2], p[2], p[2], p[2] };
    pager_set_order(pg, dups, 8);
    CHECK(pager_count(pg) == 1 && pager_page(pg, 0) == p[2], "count %d", pager_count(pg));
    bool all = true;
    for (int i = 0; i < 5; i++) all &= pager_index(pg, p[i]) >= 0;
    CHECK(all, "every page still in the pager");

    // vertical: positions down the screen
    lv_obj_t *vp = pager_create(scr, true, 3, NULL, NULL, NULL);
    lv_obj_t *v2 = pager_page(vp, 2), *v0 = pager_page(vp, 0);
    lv_obj_t *vw[] = { v2, v0 };
    pager_set_order(vp, vw, 2);
    CHECK(lv_obj_get_y(v2) == 0 && lv_obj_get_y(v0) == H && lv_obj_get_x(v0) == 0, "vertical positions");

    // a pager on a page of another (esp32-s3-rtcquebec: the stops, vertical, in the middle of alerts | stops | map):
    // on view only while that page is the one shown, all the way up
    lv_obj_t *scr2 = lv_obj_create(NULL);
    lv_obj_t *row = pager_create(scr2, false, 3, NULL, NULL, NULL);
    lv_obj_t *col = pager_create(pager_page(row, 1), true, 4, NULL, NULL, NULL);
    lv_obj_t *deep = pager_create(pager_page(col, 2), false, 2, NULL, NULL, NULL);
    pager_go(row, 0, false);
    CHECK(pager_on_view(row) && !pager_on_view(col), "row's page 0 shown: the column is off view");
    pager_go(row, 1, false);
    CHECK(pager_on_view(col), "row's page 1 shown: the column is on view");
    CHECK(!pager_on_view(deep), "the column shows its page 0, not the page holding the third pager");
    pager_go(col, 2, false);
    CHECK(pager_on_view(deep), "all the way up");
    pager_go(row, 2, false);
    CHECK(!pager_on_view(deep) && !pager_on_view(col), "the row moved on: neither below it is on view");
    pager_go(row, 1, false);
    lv_obj_add_flag(col, LV_OBJ_FLAG_HIDDEN);
    CHECK(!pager_on_view(col) && !pager_on_view(deep), "hidden");
    lv_obj_remove_flag(col, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *plain = lv_obj_create(scr2), *inside = lv_obj_create(plain);
    lv_obj_t *under_plain = pager_create(inside, true, 2, NULL, NULL, NULL);
    CHECK(pager_on_view(under_plain), "inside plain objects (not pager pages): on view");
    return check_done("pager");
}
