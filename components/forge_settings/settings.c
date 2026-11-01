// The Settings screen (forge_settings.h). Ported from weather_amoled's cfg_* (main/ui.c, v1.15.0): the same layout,
// rows and brightness band, with the rows given by the app instead of a fixed enum.
#include "forge_settings.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "forge_i18n.h"
#include "presence.h"
#include "slide.h"
#include "web.h"

// How a screen comes in: forge_lvgl's slide_to, or the app's own (weather_amoled keeps its own slide.c)
static void (*settings_slider)(lv_obj_t *scr, int dir, bool vertical) = slide_to;
void settings_set_slider(void (*fn)(lv_obj_t *scr, int dir, bool vertical)) { settings_slider = fn ? fn : slide_to; }

static const char *TAG = "settings";

#define C_BG     lv_color_hex(0x000000)
#define C_TEXT   lv_color_hex(0xF2F4F7)
#define C_DIM    lv_color_hex(0x8B95A1)
#define C_ACCENT lv_color_hex(0x4DA3FF)
#define C_ROW    lv_color_hex(0x1A2027)               // a row while pressed
#define C_KNOB   lv_color_hex(0x2A3138)               // Done, a switch that is off, the arc's track
#define C_BRIGHT lv_color_hex(0xFFC83D)

#define ROW_W 300
#define ROW_H 52
#define INFO_H 40
#define BOX_Y 70
#define BAND_Y 364                                    // the brightness band: below this, the finger's x sets it
#define BR_X0 60
#define BR_X1 406

static settings_opts_t O;
static settings_row_t R[SETTINGS_ROWS_MAX];
static lv_obj_t *row_obj[SETTINGS_ROWS_MAX], *row_name[SETTINGS_ROWS_MAX], *row_val[SETTINGS_ROWS_MAX];
static int nrows;
static lv_obj_t *scr, *box, *done_lbl, *arc, *bright, *zone;
static lv_obj_t *scr_phone, *ph_title, *ph_qr, *ph_note;
static lv_obj_t *back_to;                             // the screen Settings was opened from
static bool away;                                     // an action row opened a screen of the app's (settings_resume)

const char *settings_text(settings_text_t code)
{
    return code >= 0 && code < SET_T_COUNT && O.texts ? i18n_text(O.texts[code]) : "";
}

static void set_text(lv_obj_t *l, const char *s)      // only when it changes: a redraw that changes nothing costs a frame
{
    if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

static void set_hidden(lv_obj_t *o, bool hide)
{
    if (hide != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static int text_w(const char *s, const lv_font_t *f)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x + 1;
}

// A label cut with "..." to at most w: its width set from its text (LESSONS L194: one sized to its content shows "...")
static void fit(lv_obj_t *l, const char *s, int w)
{
    int want = text_w(s, lv_obj_get_style_text_font(l, 0));
    if (want > w) want = w;
    if (lv_obj_get_style_width(l, 0) != want) lv_obj_set_width(l, want);
    set_text(l, s);
}

/* ---------- rows ---------- */

static void refresh_row(int i)
{
    const settings_row_t *r = &R[i];
    bool shown = !r->shown || r->shown(r->user);
    set_hidden(row_obj[i], !shown);
    if (!shown) return;
    const char *name = i18n_text(r->label);
    if (r->kind == SET_SECTION) { set_text(row_name[i], name); return; }
    if (r->kind == SET_SWITCH) {
        bool on = r->on && r->on(r->user);
        if (on != lv_obj_has_state(row_val[i], LV_STATE_CHECKED)) {
            if (on) lv_obj_add_state(row_val[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(row_val[i], LV_STATE_CHECKED);
        }
        fit(row_name[i], name, ROW_W - 28 - 54 - 12);
        return;
    }
    char v[64] = "";
    if (r->value) r->value(r->user, v, sizeof(v));
    // The name first, then the value in what is left (at least a third of the row)
    int nw = text_w(name, lv_obj_get_style_text_font(row_name[i], 0));
    int vmax = ROW_W - 28 - 12 - nw;
    if (vmax < ROW_W / 3) vmax = ROW_W / 3;
    fit(row_val[i], v, vmax);
    fit(row_name[i], name, ROW_W - 28 - 12 - lv_obj_get_style_width(row_val[i], 0));
}

void settings_refresh(void)
{
    if (!scr) return;
    set_text(done_lbl, settings_text(SET_T_DONE));
    for (int i = 0; i < nrows; i++) refresh_row(i);
    if (O.brightness && !lv_obj_has_state(zone, LV_STATE_PRESSED)) {
        presence_cfg_t c;
        presence_get_config(&c);
        lv_arc_set_value(arc, c.bright_pct);                   // (returns at once when the value is the same)
        char b[48];
        snprintf(b, sizeof(b), settings_text(SET_T_BRIGHTNESS), c.bright_pct);
        set_text(bright, b);
    }
}

static void tick(lv_timer_t *t)
{
    if (lv_screen_active() == scr) settings_refresh();      // states, changes made from the phone
}

static void row_tapped(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "row %d", i);
    if (R[i].tap) R[i].tap(R[i].user);
    if (lv_screen_active() == scr || lv_screen_active() == NULL) settings_refresh();
}

static lv_obj_t *add_row(int i)
{
    const settings_row_t *r = &R[i];
    lv_obj_t *row = row_obj[i] = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *l = row_name[i] = lv_label_create(row);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, "");
    if (r->kind == SET_SECTION) {
        lv_obj_set_size(row, ROW_W - 10, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_top(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(l, O.font_small, 0);
        lv_obj_set_style_text_color(l, C_DIM, 0);
        lv_obj_set_width(l, ROW_W - 10);
        return row;
    }
    bool info = r->kind == SET_INFO;
    lv_obj_set_size(row, ROW_W, info ? INFO_H : ROW_H);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, C_ROW, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    if (info) lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    else {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_tapped, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_set_style_text_font(l, info ? O.font_small : O.font, 0);
    lv_obj_set_style_text_color(l, info ? C_DIM : C_TEXT, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 14, 0);
    if (r->kind == SET_SWITCH) {
        lv_obj_t *sw = row_val[i] = lv_switch_create(row);
        lv_obj_set_size(sw, 54, 30);
        lv_obj_set_style_bg_color(sw, C_KNOB, 0);
        lv_obj_set_style_bg_color(sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);         // the whole row is the button
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    } else {
        lv_obj_t *v = row_val[i] = lv_label_create(row);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(v, info ? O.font_small : O.font, 0);
        lv_obj_set_style_text_color(v, info ? C_TEXT : C_ACCENT, 0);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_label_set_text(v, "");
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    return row;
}

/* ---------- brightness ----------
 * The band under the arc follows the finger's x (left 5 %, right 100 %); the arc only shows the value (weather_amoled:
 * a clickable full-size lv_arc caught every touch on the screen, rows and Done included). */

static void bright_changed(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    int v = 5 + (pt.x - BR_X0) * 95 / (BR_X1 - BR_X0);
    v = v < 5 ? 5 : v > 100 ? 100 : v;
    lv_arc_set_value(arc, v);
    char b[48];
    snprintf(b, sizeof(b), settings_text(SET_T_BRIGHTNESS), v);
    set_text(bright, b);
    presence_preview_brightness(v);                         // the screen follows the finger
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {   // saved when the finger lifts (an NVS write)
        presence_cfg_t c;
        presence_get_config(&c);
        c.bright_pct = v;
        presence_set_config(&c);
        ESP_LOGI(TAG, "brightness %d%%", v);
    }
}

/* ---------- open, close ---------- */

static void wait_release(void)
{
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);                      // the press that got here isn't also a tap on a row
}

void settings_open(void)
{
    if (!scr) return;
    lv_obj_t *from = lv_screen_active();
    if (from != scr && from != scr_phone) back_to = from;
    away = false;
    ESP_LOGI(TAG, "open");
    settings_refresh();
    lv_obj_scroll_to_y(box, 0, LV_ANIM_OFF);
    settings_slider(scr, 1, true);                          // up from below
    wait_release();
}

void settings_close(void)
{
    if (!scr || !back_to) return;
    ESP_LOGI(TAG, "closed");
    away = false;
    if (lv_screen_active() == scr) settings_slider(back_to, -1, true);
    else lv_screen_load(back_to);
    if (O.closed) O.closed();
}

bool settings_shown(void) { return scr && (lv_screen_active() == scr || lv_screen_active() == scr_phone); }
lv_obj_t *settings_screen(void) { return scr; }

void settings_leave_for(void) { away = true; }

bool settings_resume(void)
{
    if (!scr || !away) return false;
    away = false;
    ESP_LOGI(TAG, "back from a page it opened");
    settings_refresh();
    lv_screen_load(scr);
    return true;
}

void settings_scroll(int page)
{
    if (!scr) return;
    settings_refresh();
    lv_obj_update_layout(box);
    lv_obj_scroll_to_y(box, 0, LV_ANIM_OFF);
    int max = lv_obj_get_scroll_bottom(box), y = page * (lv_obj_get_height(box) - 40);
    lv_obj_scroll_to_y(box, y < max ? y : max, LV_ANIM_OFF);
}

void settings_show_page(int page)
{
    if (!scr) return;
    settings_scroll(page);
    if (lv_screen_active() != scr) {
        lv_obj_t *from = lv_screen_active();
        if (from != scr_phone) back_to = from;
        lv_screen_load(scr);
    }
}

static void done_clicked(lv_event_t *e) { settings_close(); }

static void gesture(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    if (lv_indev_get_gesture_dir(in) == LV_DIR_RIGHT) settings_close();   // swipe right = back
    lv_indev_wait_release(in);
}

/* ---------- the phone's settings page: a QR code ---------- */

static void phone_back(lv_event_t *e)
{
    settings_refresh();
    settings_slider(scr, -1, false);
    wait_release();
}

void settings_show_phone(void)
{
    char url[96];
    bool have = web_url(url, sizeof(url));
    set_text(ph_title, settings_text(SET_T_PHONE));
    set_text(ph_note, settings_text(have ? SET_T_PHONE_SCAN : SET_T_PHONE_NONE));
    static char qr_text[96];
    if (have && strcmp(url, qr_text)) {
        strlcpy(qr_text, url, sizeof(qr_text));
        lv_qrcode_update(ph_qr, qr_text, strlen(qr_text));
    }
    set_hidden(ph_qr, !have);
    ESP_LOGI(TAG, "phone settings page%s", have ? "" : ": no address yet");
    settings_slider(scr_phone, 1, false);
    wait_release();
}

static lv_obj_t *base_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, C_BG, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t *centred(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, w);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, "");
    return l;
}

static void phone_create(void)
{
    scr_phone = base_screen();
    lv_obj_add_flag(scr_phone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_phone, phone_back, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_phone, phone_back, LV_EVENT_GESTURE, NULL);
    ph_title = centred(scr_phone, O.font, C_ACCENT, 48, 300);
    ph_qr = lv_qrcode_create(scr_phone);
    lv_qrcode_set_size(ph_qr, 200);
    lv_qrcode_set_dark_color(ph_qr, lv_color_black());
    lv_qrcode_set_light_color(ph_qr, lv_color_white());
    lv_obj_set_style_border_color(ph_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(ph_qr, 8, 0);
    lv_obj_remove_flag(ph_qr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(ph_qr, LV_ALIGN_TOP_MID, 0, 100);
    ph_note = centred(scr_phone, O.font_small, C_DIM, 336, 300);
}

/* ---------- creation ---------- */

lv_obj_t *settings_create(const settings_opts_t *opts, const settings_row_t *rows, int n)
{
    O = *opts;
    nrows = n < SETTINGS_ROWS_MAX ? n : SETTINGS_ROWS_MAX;
    memcpy(R, rows, nrows * sizeof(R[0]));
    for (int i = 0; i < nrows; i++)                         // a ready-made row's label: a code, the app's id from texts
        if (R[i].label < 0) R[i].label = O.texts ? O.texts[-1 - R[i].label] : 0;
    scr = base_screen();
    lv_obj_t *done = lv_button_create(scr);                 // top: Done
    lv_obj_set_size(done, 120, 40);
    lv_obj_set_style_radius(done, 20, 0);
    lv_obj_set_style_bg_color(done, C_KNOB, 0);
    lv_obj_set_style_shadow_width(done, 0, 0);
    lv_obj_align(done, LV_ALIGN_TOP_MID, 0, 22);
    done_lbl = lv_label_create(done);
    lv_obj_set_style_text_font(done_lbl, O.font, 0);
    lv_obj_set_style_text_color(done_lbl, C_TEXT, 0);
    lv_label_set_text(done_lbl, "");
    lv_obj_center(done_lbl);
    lv_obj_add_event_cb(done, done_clicked, LV_EVENT_CLICKED, NULL);

    box = lv_obj_create(scr);                               // the rows scroll; Done and the brightness band stay
    lv_obj_remove_style_all(box);
    // Without the band the list goes lower: the round screen is still ~300 px wide at y 410
    lv_obj_set_size(box, ROW_W, (O.brightness ? BAND_Y - 4 : 410) - BOX_Y);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, BOX_Y);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_style_pad_bottom(box, 12, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(box, LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (int i = 0; i < nrows; i++) add_row(i);

    if (O.brightness) {                                     // an arc along the bottom edge, the band under it
        bright = centred(scr, O.font_small, C_DIM, 372, 200);
        arc = lv_arc_create(scr);
        int32_t w = lv_display_get_horizontal_resolution(NULL);
        lv_obj_set_size(arc, w - 14, w - 14);
        lv_obj_center(arc);
        lv_arc_set_bg_angles(arc, 35, 145);
        lv_arc_set_mode(arc, LV_ARC_MODE_REVERSE);            // towards the right = brighter
        lv_arc_set_range(arc, 5, 100);
        lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, C_KNOB, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, C_BRIGHT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(arc, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(arc, 6, LV_PART_KNOB);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);       // shows the value only (bright_changed)
        zone = lv_obj_create(scr);
        lv_obj_remove_style_all(zone);
        lv_obj_set_size(zone, w, lv_display_get_vertical_resolution(NULL) - BAND_Y);
        lv_obj_align(zone, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(zone, bright_changed, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(zone, bright_changed, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(zone, bright_changed, LV_EVENT_PRESS_LOST, NULL);
    }
    lv_obj_add_event_cb(scr, gesture, LV_EVENT_GESTURE, NULL);
    phone_create();
    lv_timer_create(tick, 1000, NULL);
    settings_refresh();
    return scr;
}
