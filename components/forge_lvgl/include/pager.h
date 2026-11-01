#pragma once
#include <stdbool.h>
#include "lvgl.h"

// Full-screen pager: pages side by side (horizontal) or stacked (vertical) in a scroller that snaps one page at a
// time, so a page follows the finger, snaps into place, and bounces at the first and last page (LVGL elastic
// scrolling). (In weather_amoled the hourly view and the weather screen used it; their drags became pictures, slide.c,
// for 60 fps: see docs/LESSONS.md, LVGL.)
//
// Pages are plain containers: not clickable, presses and gestures bubble up through the pager (which stays
// clickable so it can scroll) to the screen. A gesture across the pager's direction still reaches the screen.

typedef void (*pager_cb_t)(int page, void *user);

// on_change: while dragging, each time another page reaches the middle (page dots).
// on_settle: when scrolling stops, with the page it settled on.
lv_obj_t *pager_create(lv_obj_t *parent, bool vertical, int pages, pager_cb_t on_change, pager_cb_t on_settle,
                       void *user);
lv_obj_t *pager_page(lv_obj_t *pager, int i);
void pager_go(lv_obj_t *pager, int i, bool anim);   // show page i
int pager_current(lv_obj_t *pager);                // the page in the middle now
int pager_count(lv_obj_t *pager);
// Show only the first n pages (1..created); the others are hidden and can't be reached. pager_page() still gives
// every created page.
void pager_set_count(lv_obj_t *pager, int n);
// Page i only if it is shown (i < pager_count), else NULL: the neighbours a drag may reach.
lv_obj_t *pager_shown(lv_obj_t *pager, int i);
// Show these pages, in this order (each a page of this pager, from pager_page()); the other pages follow, hidden.
// For a page that must stay last whatever comes before it (an app's summary after a varying list of pages).
// The index of a page changes with the order: keep the page object, not its number (pager_index finds it).
void pager_set_order(lv_obj_t *pager, lv_obj_t *const *pages, int n);
bool pager_vertical(lv_obj_t *pager);
// A pager can sit on a page of another pager across it (a vertical list of stops in the middle of a row of screens):
// each axis drags its own (slide.c). On view: every pager page above it is the one its pager shows, nothing hidden.
bool pager_on_view(lv_obj_t *pager);
int pager_index(lv_obj_t *pager, const lv_obj_t *page);   // -1: not one of its pages

// For drags drawn as pictures (outside LVGL): the finger no longer scrolls the pager (pager_freeze); a page's picture is
// taken with the pager moved there and back within one LVGL cycle (pager_peek: no callbacks); the drag ends with
// pager_switch (on_change and on_settle, like a scroll that settled there).
void pager_freeze(lv_obj_t *pager);
void pager_peek(lv_obj_t *pager, int i);
void pager_switch(lv_obj_t *pager, int i);
