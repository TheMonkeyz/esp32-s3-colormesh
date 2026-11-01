// Snapping full-screen pager (see pager.h)
#include "pager.h"
#include "lvgl.h"
#define DISP_W lv_display_get_horizontal_resolution(NULL)
#define DISP_H lv_display_get_vertical_resolution(NULL)

typedef struct {
    bool vertical, quiet;              // quiet: moved by pager_peek / pager_switch, no callbacks from the scroll events
    int pages, cur;
    int max;                           // pages created (pager_set_count shows the first `pages` of them)
    pager_cb_t on_change, on_settle;
    void *user;
    lv_obj_t *page[];
} pager_t;

// The pagers alive, so pager_on_view() can tell a pager from any other object (user data alone can't)
#define MAX_LIVE 8
static lv_obj_t *live[MAX_LIVE];

static bool is_pager(const lv_obj_t *o)
{
    for (int i = 0; i < MAX_LIVE; i++) if (o && live[i] == o) return true;
    return false;
}

static int page_at(lv_obj_t *o, const pager_t *p)
{
    int pos = p->vertical ? lv_obj_get_scroll_y(o) : lv_obj_get_scroll_x(o);
    int size = p->vertical ? DISP_H : DISP_W;
    int i = (pos + size / 2) / size;
    return i < 0 ? 0 : i >= p->pages ? p->pages - 1 : i;
}

static void scrolled(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    if (lv_event_get_target(e) != o) return;   // a page's own list scrolling (bubbled up), not the pager
    pager_t *p = lv_obj_get_user_data(o);
    if (p->quiet) return;
    int i = page_at(o, p);
    if (i != p->cur) {
        p->cur = i;
        if (p->on_change) p->on_change(i, p->user);
    }
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_END && p->on_settle) p->on_settle(i, p->user);
}

static void deleted(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    if (o != lv_event_get_current_target(e)) return;
    for (int i = 0; i < MAX_LIVE; i++) if (live[i] == o) live[i] = NULL;
    lv_free(lv_obj_get_user_data(o));
}

lv_obj_t *pager_create(lv_obj_t *parent, bool vertical, int pages, pager_cb_t on_change, pager_cb_t on_settle,
                       void *user)
{
    pager_t *p = lv_malloc_zeroed(sizeof(pager_t) + pages * sizeof(lv_obj_t *));
    *p = (pager_t){ .vertical = vertical, .pages = pages, .max = pages, .on_change = on_change, .on_settle = on_settle,
                    .user = user };
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, DISP_W, DISP_H);
    lv_obj_set_user_data(o, p);
    lv_obj_set_scroll_dir(o, vertical ? LV_DIR_VER : LV_DIR_HOR);
    if (vertical) lv_obj_set_scroll_snap_y(o, LV_SCROLL_SNAP_CENTER);
    else lv_obj_set_scroll_snap_x(o, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ONE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(o, scrolled, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(o, scrolled, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(o, deleted, LV_EVENT_DELETE, NULL);
    for (int i = 0; i < MAX_LIVE; i++) if (!live[i]) { live[i] = o; break; }
    for (int i = 0; i < pages; i++) {
        lv_obj_t *pg = p->page[i] = lv_obj_create(o);
        lv_obj_remove_style_all(pg);
        lv_obj_set_size(pg, DISP_W, DISP_H);
        lv_obj_set_pos(pg, vertical ? 0 : i * DISP_W, vertical ? i * DISP_H : 0);
        lv_obj_remove_flag(pg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pg, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    return o;
}

lv_obj_t *pager_page(lv_obj_t *o, int i)
{
    pager_t *p = lv_obj_get_user_data(o);
    return i >= 0 && i < p->max ? p->page[i] : NULL;
}

void pager_go(lv_obj_t *o, int i, bool anim)
{
    pager_t *p = lv_obj_get_user_data(o);
    lv_obj_update_layout(o);
    if (p->vertical) lv_obj_scroll_to_y(o, i * DISP_H, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    else lv_obj_scroll_to_x(o, i * DISP_W, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    if (!anim) p->cur = i;
}

int pager_current(lv_obj_t *o)
{
    return page_at(o, lv_obj_get_user_data(o));
}

int pager_count(lv_obj_t *o) { return ((pager_t *)lv_obj_get_user_data(o))->pages; }

// The pages past n are hidden: out of the scroll range (LVGL skips hidden children) and past slide.c's drags (it stops
// at pager_count); they keep their objects, so an app builds all of them once and shows as many as it has data for
void pager_set_count(lv_obj_t *o, int n)
{
    pager_t *p = lv_obj_get_user_data(o);
    n = n < 1 ? 1 : n > p->max ? p->max : n;
    for (int i = 0; i < p->max; i++) {
        if (i < n) lv_obj_remove_flag(p->page[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(p->page[i], LV_OBJ_FLAG_HIDDEN);
    }
    p->pages = n;
    if (p->cur >= n) pager_go(o, n - 1, false);
}

lv_obj_t *pager_shown(lv_obj_t *o, int i)
{
    pager_t *p = lv_obj_get_user_data(o);
    return i >= 0 && i < p->pages ? p->page[i] : NULL;
}

// The pages asked for move to the front, in that order (a page listed twice or not of this pager is skipped); the
// others follow in their current order, hidden. Positions follow the new order, so the scroll range and slide.c's
// neighbours (pager_shown) see it at once.
void pager_set_order(lv_obj_t *o, lv_obj_t *const *pages, int n)
{
    pager_t *p = lv_obj_get_user_data(o);
    lv_obj_t *all[p->max];
    int k = 0;
    for (int i = 0; i < n && k < p->max; i++) {
        bool dup = pager_index(o, pages[i]) < 0;
        for (int j = 0; j < k && !dup; j++) dup = all[j] == pages[i];
        if (!dup) all[k++] = pages[i];
    }
    int shown = k;
    for (int i = 0; i < p->max; i++) {
        bool listed = false;
        for (int j = 0; j < shown && !listed; j++) listed = all[j] == p->page[i];
        if (!listed) all[k++] = p->page[i];
    }
    for (int i = 0; i < p->max; i++) {
        p->page[i] = all[i];
        lv_obj_set_pos(all[i], p->vertical ? 0 : i * DISP_W, p->vertical ? i * DISP_H : 0);
    }
    pager_set_count(o, shown);
}

int pager_index(lv_obj_t *o, const lv_obj_t *page)
{
    pager_t *p = lv_obj_get_user_data(o);
    for (int i = 0; i < p->max; i++) if (p->page[i] == page) return i;
    return -1;
}

static void move(lv_obj_t *o, pager_t *p, int i)
{
    p->quiet = true;
    lv_obj_update_layout(o);
    if (p->vertical) lv_obj_scroll_to_y(o, i * DISP_H, LV_ANIM_OFF);
    else lv_obj_scroll_to_x(o, i * DISP_W, LV_ANIM_OFF);
    p->quiet = false;
}

void pager_peek(lv_obj_t *o, int i) { move(o, lv_obj_get_user_data(o), i); }

void pager_switch(lv_obj_t *o, int i)
{
    pager_t *p = lv_obj_get_user_data(o);
    move(o, p, i);
    p->cur = i;
    if (p->on_change) p->on_change(i, p->user);
    if (p->on_settle) p->on_settle(i, p->user);
}

void pager_freeze(lv_obj_t *o) { lv_obj_set_scroll_dir(o, LV_DIR_NONE); }
bool pager_vertical(lv_obj_t *o) { return ((pager_t *)lv_obj_get_user_data(o))->vertical; }

// Up from the pager: each pager page on the way must be the one its pager shows (and nothing on the way hidden)
bool pager_on_view(lv_obj_t *o)
{
    for (lv_obj_t *c = o; c; ) {
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) return false;
        lv_obj_t *page = lv_obj_get_parent(c);
        lv_obj_t *outer = page ? lv_obj_get_parent(page) : NULL;
        if (is_pager(outer) && pager_shown(outer, pager_current(outer)) != page) return false;
        c = page;
    }
    return true;
}
