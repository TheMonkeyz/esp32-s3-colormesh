// The app's screens (see ui.h). Stub for now: one black screen per name with its title and the version, registered
// with the screen registry so the test console's "screen" command works. The UI agent replaces this file.
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "board.h"
#include "screens.h"
#include "app_text.h"
#include "config.h"

#define C_BG     lv_color_hex(0x000000)       // AMOLED: black pixels are off
#define C_TEXT   lv_color_hex(0xF2F4F7)
#define C_DIM    lv_color_hex(0x8B95A1)
#define C_ACCENT lv_color_hex(0x4DA3FF)

extern const uint8_t ttf_start[] asm("_binary_montserrat_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_montserrat_ttf_end");

static lv_font_t *f_mid, *f_small;
static lv_obj_t *scr_pick, *scr_main, *scr_sys, *scr_msg;
static lv_obj_t *p_title, *p_sub, *mn_hint, *mn_near, *mn_dev, *s_title, *s_ver, *m_title, *m_body;

static lv_font_t *mkfont(int px)
{
    // No kerning: it cost 71 % of render time in weather_amoled. 96 cached glyphs per size.
    return lv_tiny_ttf_create_data_ex(ttf_start, ttf_end - ttf_start, px, LV_FONT_KERNING_NONE, 96);
}

static lv_obj_t *base_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, C_BG, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

// Labels are never clickable: a decorative object that is clickable swallows presses
static lv_obj_t *label(lv_obj_t *parent, lv_font_t *f, lv_color_t c, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, w);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_label_set_text(l, "");
    return l;
}

// Set only when it changes: a redraw that changes nothing still costs a frame
static void set_text(lv_obj_t *l, const char *s)
{
    if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

static void refresh(void)                           // display lock held
{
    const char *ver = esp_app_get_description()->version;
    char buf[64];
    set_text(p_title, tr(T_PICK_COLOUR));
    set_text(p_sub, tr(T_TAP_ONE));
    set_text(mn_hint, tr(T_HOLD_HINT));
    snprintf(buf, sizeof(buf), tr(T_NEAR), 0, ver);
    set_text(mn_near, buf);
    set_text(mn_dev, config_get()->dev_mode ? tr(T_DEV) : "");
    set_text(s_title, "System");
    snprintf(buf, sizeof(buf), "colormesh %s", ver);
    set_text(s_ver, buf);
}

static void make_screens(void)
{
    scr_pick = base_screen();
    p_title = label(scr_pick, f_mid, C_ACCENT, 170, 340);
    p_sub = label(scr_pick, f_small, C_DIM, 230, 300);
    scr_main = base_screen();
    mn_hint = label(scr_main, f_small, C_DIM, 190, 320);
    mn_near = label(scr_main, f_small, C_TEXT, 330, 300);
    mn_dev = label(scr_main, f_small, C_ACCENT, 90, 120);
    scr_sys = base_screen();
    s_title = label(scr_sys, f_mid, C_ACCENT, 150, 300);
    s_ver = label(scr_sys, f_small, C_TEXT, 210, 360);
    scr_msg = base_screen();
    m_title = label(scr_msg, f_mid, C_ACCENT, 140, 320);
    m_body = label(scr_msg, f_small, C_TEXT, 200, 340);
    refresh();
}

/* ---------- screen registry (test console, snapshots) ---------- */

static lv_obj_t *get_pick(void) { return scr_pick; }
static lv_obj_t *get_main(void) { return scr_main; }
static lv_obj_t *get_sys(void) { return scr_sys; }
static lv_obj_t *get_msg(void) { return scr_msg; }
static bool shown_pick(void) { return lv_screen_active() == scr_pick; }
static bool shown_main(void) { return lv_screen_active() == scr_main; }
static bool shown_sys(void) { return lv_screen_active() == scr_sys; }
static bool shown_msg(void) { return lv_screen_active() == scr_msg; }

static const screen_def_t screens[] = {
    { "pick",    get_pick, NULL, refresh, shown_pick },
    { "main",    get_main, NULL, refresh, shown_main },
    { "system",  get_sys,  NULL, refresh, shown_sys },
    { "message", get_msg,  NULL, NULL,    shown_msg },
};

void ui_init(void)
{
    display_lock(-1);
    f_mid = mkfont(30);
    f_small = mkfont(20);
    make_screens();
    screens_register(screens, sizeof(screens) / sizeof(screens[0]));
    set_text(m_title, "ColorMesh");
    set_text(m_body, tr(T_STARTING));
    lv_screen_load(scr_msg);
    display_unlock();
}

static void show(lv_obj_t *s)
{
    display_lock(-1);
    refresh();
    if (lv_screen_active() != s) lv_screen_load(s);
    display_unlock();
}

void ui_show_pick(void) { show(scr_pick); }
void ui_show_main(void) { show(scr_main); }

void ui_message(const char *title, const char *body)
{
    display_lock(-1);
    set_text(m_title, title);
    set_text(m_body, body);
    if (lv_screen_active() != scr_msg) lv_screen_load(scr_msg);
    display_unlock();
}

void ui_texts_changed(void)
{
    display_lock(-1);
    refresh();
    display_unlock();
}
