#pragma once
// Host fake of the few LVGL calls pager.c makes (test_pager.c implements them): objects with a position, size, flags
// and a scroll position kept in the range of their shown children, as LVGL does (hidden children don't count).
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct lv_obj_t lv_obj_t;
typedef struct lv_event_t lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t *e);
typedef enum { LV_EVENT_SCROLL = 1, LV_EVENT_SCROLL_END, LV_EVENT_DELETE } lv_event_code_t;
typedef enum { LV_ANIM_OFF, LV_ANIM_ON } lv_anim_enable_t;
enum { LV_DIR_NONE = 0, LV_DIR_HOR = 3, LV_DIR_VER = 12 };
enum { LV_SCROLL_SNAP_CENTER = 3 };
enum { LV_SCROLLBAR_MODE_OFF = 0 };
enum {
    LV_OBJ_FLAG_HIDDEN = 1 << 0, LV_OBJ_FLAG_CLICKABLE = 1 << 1, LV_OBJ_FLAG_SCROLLABLE = 1 << 4,
    LV_OBJ_FLAG_SCROLL_ONE = 1 << 9, LV_OBJ_FLAG_EVENT_BUBBLE = 1 << 14, LV_OBJ_FLAG_GESTURE_BUBBLE = 1 << 15,
};

#define lv_malloc_zeroed(n) calloc(1, (n))
#define lv_free free
int32_t lv_display_get_horizontal_resolution(void *disp);
int32_t lv_display_get_vertical_resolution(void *disp);
lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_obj_get_parent(const lv_obj_t *o);
void lv_obj_remove_style_all(lv_obj_t *o);
void lv_obj_set_size(lv_obj_t *o, int32_t w, int32_t h);
void lv_obj_set_pos(lv_obj_t *o, int32_t x, int32_t y);
void lv_obj_set_user_data(lv_obj_t *o, void *user);
void *lv_obj_get_user_data(lv_obj_t *o);
void lv_obj_add_flag(lv_obj_t *o, uint32_t f);
void lv_obj_remove_flag(lv_obj_t *o, uint32_t f);
bool lv_obj_has_flag(const lv_obj_t *o, uint32_t f);
void lv_obj_set_scroll_dir(lv_obj_t *o, int dir);
void lv_obj_set_scroll_snap_x(lv_obj_t *o, int snap);
void lv_obj_set_scroll_snap_y(lv_obj_t *o, int snap);
void lv_obj_set_scrollbar_mode(lv_obj_t *o, int mode);
void lv_obj_add_event_cb(lv_obj_t *o, lv_event_cb_t cb, lv_event_code_t code, void *user);
void lv_obj_update_layout(lv_obj_t *o);
void lv_obj_scroll_to_x(lv_obj_t *o, int32_t x, lv_anim_enable_t anim);
void lv_obj_scroll_to_y(lv_obj_t *o, int32_t y, lv_anim_enable_t anim);
int32_t lv_obj_get_scroll_x(const lv_obj_t *o);
int32_t lv_obj_get_scroll_y(const lv_obj_t *o);
int32_t lv_obj_get_x(const lv_obj_t *o);
int32_t lv_obj_get_y(const lv_obj_t *o);
lv_obj_t *lv_event_get_target(lv_event_t *e);
lv_obj_t *lv_event_get_current_target(lv_event_t *e);
lv_event_code_t lv_event_get_code(lv_event_t *e);
