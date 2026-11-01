// The app's screens (see ui.h): "pick" | "main" | "system", the three pages of one pager (swiped like the espforge
// starter's hello | system: the ring of 12 colours on the left, main in the middle), and "message". Everything here
// runs in the LVGL task or under display_lock.
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "board.h"
#include "pager.h"
#include "screens.h"
#include "slide.h"
#include "app_text.h"
#include "config.h"
#include "mesh.h"
#include "mesh_fw.h"

static const char *TAG = "ui";

#define C_BG     lv_color_hex(0x000000)       // AMOLED: black pixels are off
#define C_TEXT   lv_color_hex(0xF2F4F7)
#define C_DIM    lv_color_hex(0x8B95A1)
#define C_HINT   lv_color_hex(0x5C6470)       // the dim hint on the black centre
#define C_DARK   lv_color_hex(0x1A1A1A)       // text on a bright mix
#define C_ACCENT lv_color_hex(0x4DA3FF)

#define CX (DISP_W / 2)
#define CY (DISP_H / 2)

#define RING_W     14                         // main: the own colour at the edge
#define DISC_R     215                        // main: the mix
#define DOT_R      5                          // main: one dot per peer
#define DOT_STEP   14
#define DOT_Y      372                        // centre of the dots' row
#define NEAR_Y     388                        // top of the "N near" line
#define MAX_DOTS   16
#define SW_N       12                         // pick: swatches
#define SW_R       22
#define SW_RING    180
#define SYS_ROWS   6                          // system: peers listed
#define HOLD_IDLE_MS 150                      // safety net: no finger for this long = released

extern const uint8_t ttf_start[] asm("_binary_montserrat_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_montserrat_ttf_end");

static lv_font_t *f_mid, *f_small, *f_tiny;
enum { PG_PICK, PG_MAIN, PG_SYS, PG_N };       // the pager's pages, left to right
static const char *const page_name[PG_N] = { "pick", "main", "system" };

static lv_obj_t *scr_pager, *pager, *pg_pick, *pg_main, *pg_sys, *scr_msg;
static lv_obj_t *p_title, *p_sub, *p_back, *p_sw[SW_N];
static lv_obj_t *mn_ring, *mn_disc, *mn_arc, *mn_hint, *mn_near, *mn_dev, *mn_dot[MAX_DOTS];
static lv_obj_t *s_title, *s_lines, *s_peers, *s_row[SYS_ROWS], *s_sw[SYS_ROWS];
static lv_obj_t *m_title, *m_body;
static bool ui_hold;                          // the finger on the main page started a hold (not the console)

// Hue 0 = red at the top, clockwise every 30 degrees, full saturation
static const uint32_t sw_rgb[SW_N] = {
    0xFF0000, 0xFF8000, 0xFFFF00, 0x80FF00, 0x00FF00, 0x00FF80,
    0x00FFFF, 0x0080FF, 0x0000FF, 0x8000FF, 0xFF00FF, 0xFF0080,
};

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

// A plain shape (circle, ring): no theme style, never clickable, presses go to the parent
static lv_obj_t *shape(lv_obj_t *parent, int size)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    return o;
}

// Set only when it changes: a redraw that changes nothing still costs a frame (and invalidates cached pictures)
static void set_text(lv_obj_t *l, const char *s)
{
    if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hidden) {
        if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t)r << 16 | (uint32_t)g << 8 | b; }

void ui_fmt_ver(const uint8_t ver[4], char *out, int n)
{
    int k = ver[3], m = snprintf(out, n, "v%u.%u.%u", ver[0], ver[1], ver[2]);
    if (m < 0 || m >= n) return;
    if (k < 100) snprintf(out + m, n - m, "-%d", k);
    else if (k < 200) snprintf(out + m, n - m, "-rc.%d", k - 100);
    else if (k > 200) snprintf(out + m, n - m, "+%d", k - 201);
}

/* ---------- pick ---------- */

static uint32_t pick_shown_rgb = 0xFFFFFFFFu;   // the colour the outline was drawn for

static void pick_update(void)                    // the saved colour's swatch gets the white outline
{
    const app_cfg_t *c = config_get();
    uint32_t v = rgb(c->r, c->g, c->b);
    if (v == pick_shown_rgb) return;
    pick_shown_rgb = v;
    for (int i = 0; i < SW_N; i++) lv_obj_set_style_outline_width(p_sw[i], sw_rgb[i] == v ? 3 : 0, 0);
}

static void pick_refresh(void)
{
    set_text(p_title, tr(T_PICK_COLOUR));
    set_text(p_sub, tr(T_TAP_ONE));
    set_text(p_back, tr(T_SWIPE_BACK));
    pick_update();
}

static void main_update(bool texts);

static void to_main(void *unused)            // slide_change's change: the pager on main (on_settle: "page main")
{
    main_update(true);
    pager_switch(pager, PG_MAIN);
}

static void swatch_tapped(lv_event_t *e)
{
    // A tap during a move or right after one is the next quick swipe's press, not a pick (slide.h); a tap while the
    // ring is not the page shown can't happen, but would pick blind
    if (!slide_tap_ok() || lv_screen_active() != scr_pager || pager_current(pager) != PG_PICK) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    uint8_t r = sw_rgb[i] >> 16, g = sw_rgb[i] >> 8 & 0xFF, b = sw_rgb[i] & 0xFF;
    ESP_LOGI(TAG, "picked #%06lx", (unsigned long)sw_rgb[i]);
    config_set_color(r, g, b);
    mesh_set_color(r, g, b);
    pick_update();
    slide_change(1, false, to_main, NULL);   // the ring slides out to the left, main comes in from the right
}

static void pick_create_page(lv_obj_t *pg)
{
    for (int i = 0; i < SW_N; i++) {
        float a = i * 30.0f * (float)M_PI / 180.0f;
        int x = CX + (int)lroundf(SW_RING * sinf(a)), y = CY - (int)lroundf(SW_RING * cosf(a));
        lv_obj_t *s = p_sw[i] = lv_obj_create(pg);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 2 * SW_R, 2 * SW_R);
        lv_obj_set_pos(s, x - SW_R, y - SW_R);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(sw_rgb[i]), 0);
        lv_obj_set_style_outline_color(s, lv_color_white(), 0);
        lv_obj_set_style_outline_pad(s, 4, 0);
        lv_obj_set_style_outline_width(s, 0, 0);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s, 8);              // 22 px is small for a finger; neighbours are 94 px apart
        lv_obj_add_event_cb(s, swatch_tapped, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *box = lv_obj_create(pg);                // the three lines, centred together whatever wraps
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 280, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 12, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(box);
    p_title = label(box, f_mid, C_TEXT, 0, 270);
    p_sub = label(box, f_small, C_DIM, 0, 260);
    p_back = label(box, f_tiny, C_HINT, 0, 220);
    pick_refresh();
}

/* ---------- main page ---------- */

// The last values drawn (UINT32_MAX / -2: nothing drawn yet)
static struct {
    uint32_t own, mix;
    bool dev;
    int prog, npeers;
    uint32_t dots[MAX_DOTS];          // rgb | holding << 24
} last = { .own = UINT32_MAX, .mix = UINT32_MAX, .prog = -2, .npeers = -1 };

static bool bright(uint32_t v)       // text on top of it should be dark
{
    int r = v >> 16, g = v >> 8 & 0xFF, b = v & 0xFF;
    return r * 299 + g * 587 + b * 114 > 130 * 1000;   // green (149 685) is bright, red (76 245) not
}

static void main_update(bool texts)
{
    const app_cfg_t *c = config_get();
    uint32_t own = rgb(c->r, c->g, c->b);
    if (own != last.own) {
        last.own = own;
        lv_obj_set_style_border_color(mn_ring, lv_color_hex(own), 0);
    }
    uint8_t r, g, b;
    mesh_get_mix(&r, &g, &b);
    uint32_t mix = rgb(r, g, b);
    if (mix != last.mix) {
        last.mix = mix;
        lv_obj_set_style_bg_color(mn_disc, lv_color_hex(mix), 0);
        set_hidden(mn_hint, mix != 0);
        lv_obj_set_style_text_color(mn_near, !mix ? C_DIM : bright(mix) ? C_DARK : C_TEXT, 0);
        for (int i = 0; i < MAX_DOTS; i++) lv_obj_set_style_outline_color(mn_dot[i], bright(mix) ? C_DARK : C_BG, 0);
    }
    if (c->dev_mode != last.dev) {
        last.dev = c->dev_mode;
        set_hidden(mn_dev, !c->dev_mode);
    }
    int prog = mesh_fw_progress();
    if (prog != last.prog) {
        last.prog = prog;
        set_hidden(mn_arc, prog < 0);
        if (prog >= 0) lv_arc_set_value(mn_arc, prog > 100 ? 100 : prog);
    }
    static mesh_peer_t peers[MAX_DOTS];
    int n = mesh_peers(peers, MAX_DOTS);
    if (n < 0) n = 0;
    if (n > MAX_DOTS) n = MAX_DOTS;
    bool moved = n != last.npeers;
    for (int i = 0; i < MAX_DOTS; i++) {
        uint32_t v = i < n ? rgb(peers[i].r, peers[i].g, peers[i].b) | (uint32_t)peers[i].holding << 24 : 0;
        if (!moved && v == last.dots[i]) continue;
        last.dots[i] = v;
        lv_obj_t *d = mn_dot[i];
        if (i >= n) { set_hidden(d, true); continue; }
        lv_obj_set_style_bg_color(d, lv_color_hex(v & 0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(d, peers[i].holding ? LV_OPA_COVER : 64 /* 25 % */, 0);
        lv_obj_set_pos(d, CX + (2 * i - (n - 1)) * DOT_STEP / 2 - DOT_R, DOT_Y - DOT_R);
        set_hidden(d, false);
    }
    if (moved || texts) {
        last.npeers = n;
        char buf[64];
        snprintf(buf, sizeof(buf), tr(T_NEAR), n, esp_app_get_description()->version);
        set_text(mn_near, buf);
    }
    if (texts) {
        set_text(mn_hint, tr(T_HOLD_HINT));
        set_text(mn_dev, tr(T_DEV));
    }
}

static void main_refresh(void) { main_update(true); }

static void hold_set(bool on)
{
    if (on == ui_hold) return;
    ui_hold = on;
    mesh_set_holding(on);                     // a flag for the mesh task; no radio work here
}

static void main_pressed(lv_event_t *e) { hold_set(true); }
static void main_released(lv_event_t *e) { hold_set(false); }

// slide.c: a press became a pager drag (10 px). A swipe that started on main had sent "hold on" with its press; LVGL
// sends no PRESS_LOST then, so the hold ends here instead of in fast_tick after the slide (~0.4 s of colour on the other
// boards for every swipe). Only a hold from a finger: the console's "hold on" stays.
static void drag_started(lv_obj_t *pg, void *user)
{
    if (!ui_hold) return;
    ESP_LOGI(TAG, "hold released (drag)");
    hold_set(false);
}

static void main_create_page(lv_obj_t *pg)
{
    lv_obj_add_flag(pg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(pg, main_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(pg, main_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(pg, main_released, LV_EVENT_PRESS_LOST, NULL);

    mn_ring = shape(pg, DISP_W);
    lv_obj_set_style_border_width(mn_ring, RING_W, 0);
    lv_obj_set_style_border_opa(mn_ring, LV_OPA_COVER, 0);
    lv_obj_center(mn_ring);
    mn_disc = shape(pg, 2 * DISC_R);
    lv_obj_set_style_bg_opa(mn_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(mn_disc, C_BG, 0);
    lv_obj_center(mn_disc);

    mn_arc = lv_arc_create(pg);               // the firmware pull's progress, over the ring
    lv_obj_set_size(mn_arc, DISP_W, DISP_H);
    lv_obj_center(mn_arc);
    lv_arc_set_rotation(mn_arc, 270);
    lv_arc_set_bg_angles(mn_arc, 0, 360);
    lv_arc_set_range(mn_arc, 0, 100);
    lv_arc_set_value(mn_arc, 0);
    lv_obj_set_style_arc_opa(mn_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mn_arc, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mn_arc, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(mn_arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(mn_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(mn_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(mn_arc, 0, LV_PART_MAIN);
    lv_obj_remove_flag(mn_arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mn_arc, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_HIDDEN);

    mn_hint = label(pg, f_small, C_HINT, 0, 280);
    lv_obj_align(mn_hint, LV_ALIGN_CENTER, 0, 0);
    mn_dev = label(pg, f_tiny, C_BG, 40, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(mn_dev, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(mn_dev, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mn_dev, 10, 0);
    lv_obj_set_style_pad_hor(mn_dev, 12, 0);
    lv_obj_set_style_pad_ver(mn_dev, 3, 0);
    lv_obj_add_flag(mn_dev, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < MAX_DOTS; i++) {
        lv_obj_t *d = mn_dot[i] = shape(pg, 2 * DOT_R);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_width(d, 1, 0);
        lv_obj_set_style_outline_color(d, C_BG, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
    mn_near = label(pg, f_tiny, C_DIM, NEAR_Y, 220);
    main_update(true);
}

/* ---------- system page ---------- */

static void fmt_uptime(char *out, int n)
{
    int64_t s = esp_timer_get_time() / 1000000;
    if (s < 3600) snprintf(out, n, tr(T_UP_MIN), (int)(s / 60));
    else if (s < 86400) snprintf(out, n, tr(T_UP_H), (int)(s / 3600), (int)(s / 60 % 60));
    else snprintf(out, n, tr(T_UP_D), (int)(s / 86400), (int)(s / 3600 % 24));
}

static void system_refresh(void)
{
    const app_cfg_t *c = config_get();
    char lines[400], up[24], buf[96];
    fmt_uptime(up, sizeof(up));
    const char *addr = mesh_own_addr();
    int n = snprintf(lines, sizeof(lines), tr(T_SYS_VERSION), esp_app_get_description()->version);
    n += snprintf(lines + n, sizeof(lines) - n, "\n");
    n += snprintf(lines + n, sizeof(lines) - n, tr(T_SYS_BT), addr && addr[0] ? addr : "-");
    n += snprintf(lines + n, sizeof(lines) - n, "\n");
    n += snprintf(lines + n, sizeof(lines) - n, tr(T_SYS_NAME), c->name[0] ? c->name : tr(T_SYS_NO_NAME));
    n += snprintf(lines + n, sizeof(lines) - n, "\n");
    n += snprintf(lines + n, sizeof(lines) - n, tr(T_SYS_DEV), c->dev_mode ? tr(T_ON) : tr(T_OFF));
    n += snprintf(lines + n, sizeof(lines) - n, "\n");
    n += snprintf(lines + n, sizeof(lines) - n, tr(T_SYS_MEMORY),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    n += snprintf(lines + n, sizeof(lines) - n, "\n");
    snprintf(lines + n, sizeof(lines) - n, tr(T_SYS_UPTIME), up);
    set_text(s_title, tr(T_SYSTEM));
    set_text(s_lines, lines);

    static mesh_peer_t peers[MAX_DOTS];
    int np = mesh_peers(peers, MAX_DOTS);
    if (np < 0) np = 0;
    if (np > MAX_DOTS) np = MAX_DOTS;
    if (np) snprintf(buf, sizeof(buf), tr(T_SYS_PEERS), np);
    else snprintf(buf, sizeof(buf), "%s", tr(T_SYS_NO_PEERS));
    set_text(s_peers, buf);
    for (int i = 0; i < SYS_ROWS; i++) {
        bool on = i < np;
        set_hidden(s_row[i], !on);
        set_hidden(s_sw[i], !on);
        if (!on) continue;
        const mesh_peer_t *p = &peers[i];
        char ver[24];
        ui_fmt_ver(p->ver, ver, sizeof(ver));
        snprintf(buf, sizeof(buf), "%02x:%02x  %s  %s  %d", p->addr[1], p->addr[0], ver,
                 p->holding ? tr(T_PEER_HOLD) : tr(T_PEER_IDLE), p->rssi);
        set_text(s_row[i], buf);
        lv_color_t col = lv_color_make(p->r, p->g, p->b);
        if (!lv_color_eq(lv_obj_get_style_bg_color(s_sw[i], 0), col)) lv_obj_set_style_bg_color(s_sw[i], col, 0);
    }
}

static void system_create_page(lv_obj_t *pg)
{
    s_title = label(pg, f_mid, C_ACCENT, 36, 300);
    s_lines = label(pg, f_small, C_TEXT, 80, 400);
    lv_obj_set_style_text_line_space(s_lines, 2, 0);
    s_peers = label(pg, f_small, C_ACCENT, 236, 340);
    for (int i = 0; i < SYS_ROWS; i++) {
        int y = 266 + i * 22;
        s_row[i] = label(pg, f_tiny, C_TEXT, y, 250);
        lv_obj_set_style_text_align(s_row[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(s_row[i], LV_ALIGN_TOP_LEFT, CX - 125 + 10, y);
        s_sw[i] = shape(pg, 12);
        lv_obj_set_style_bg_opa(s_sw[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(s_sw[i], CX - 125 - 10, y + 3);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sw[i], LV_OBJ_FLAG_HIDDEN);
    }
    system_refresh();
}

/* ---------- timers ---------- */

// Every 50 ms: the main page's values (only what changed is touched), the pick outline, the hold safety net. A hold
// is only ever on main: a swipe that starts there (its press already sent "hold on") ends it at the drag's start
// (drag_started); this net is for a press lost otherwise, or a page change; a press on the ring or on system starts
// none (only pg_main has the press handlers).
static void fast_tick(lv_timer_t *t)
{
    bool on_pager = lv_screen_active() == scr_pager;
    if (ui_hold && (touch_idle_ms() > HOLD_IDLE_MS || !on_pager || pager_current(pager) != PG_MAIN)) {
        ESP_LOGI(TAG, "hold released (no finger for %lu ms, or the page changed)", (unsigned long)touch_idle_ms());
        hold_set(false);
    }
    if (on_pager) {                           // every page: a swipe shows a picture of the neighbours
        main_update(false);
        pick_update();
    }
}

// Every second while the pager is shown: the system page (shown, or the neighbour a swipe will show a picture of)
static void slow_tick(lv_timer_t *t)
{
    if (lv_screen_active() == scr_pager) system_refresh();
}

static void page_settled(int page, void *user)
{
    ESP_LOGI(TAG, "page %s", page >= 0 && page < PG_N ? page_name[page] : "?");
    if (page == PG_SYS) system_refresh();
    else if (page == PG_MAIN) main_update(false);
    else pick_refresh();
}

static void pager_create_screen(void)
{
    scr_pager = base_screen();
    pager = pager_create(scr_pager, false, PG_N, NULL, page_settled, NULL);
    slide_pager(pager);                       // drags drawn as pictures (~60 fps), not LVGL scrolling
    pg_pick = pager_page(pager, PG_PICK);
    pg_main = pager_page(pager, PG_MAIN);
    pg_sys = pager_page(pager, PG_SYS);
    pick_create_page(pg_pick);
    main_create_page(pg_main);
    system_create_page(pg_sys);
}

/* ---------- message ---------- */

static void msg_create(void)
{
    scr_msg = base_screen();
    m_title = label(scr_msg, f_mid, C_ACCENT, 140, 320);
    m_body = label(scr_msg, f_small, C_TEXT, 200, 340);
}

/* ---------- showing, screen registry ---------- */

static void show_page_locked(int page)       // at once, no slide (the console's "screen", start-up)
{
    pager_go(pager, page, false);
    if (lv_screen_active() != scr_pager) lv_screen_load(scr_pager);
}

static void show_pick_locked(void)
{
    pick_refresh();
    show_page_locked(PG_PICK);
}

static void show_main_locked(void)
{
    main_update(true);
    show_page_locked(PG_MAIN);
}

static void show_system_locked(void)
{
    system_refresh();
    show_page_locked(PG_SYS);
}

static void show_msg_locked(void) { if (lv_screen_active() != scr_msg) lv_screen_load(scr_msg); }

static bool page_shown(int page) { return lv_screen_active() == scr_pager && pager_current(pager) == page; }

static lv_obj_t *get_pick(void) { return pg_pick; }
static lv_obj_t *get_main(void) { return pg_main; }
static lv_obj_t *get_sys(void) { return pg_sys; }
static lv_obj_t *get_msg(void) { return scr_msg; }
static bool shown_pick(void) { return page_shown(PG_PICK); }
static bool shown_main(void) { return page_shown(PG_MAIN); }
static bool shown_sys(void) { return page_shown(PG_SYS); }
static bool shown_msg(void) { return lv_screen_active() == scr_msg; }

static const screen_def_t screens[] = {
    { "pick",    get_pick, show_pick_locked,   pick_refresh,   shown_pick },
    { "main",    get_main, show_main_locked,   main_refresh,   shown_main },
    { "system",  get_sys,  show_system_locked, system_refresh, shown_sys },
    { "message", get_msg,  show_msg_locked,    NULL,           shown_msg },
};

void ui_init(void)
{
    display_lock(-1);
    f_mid = mkfont(30);
    f_small = mkfont(20);
    f_tiny = mkfont(14);
    pager_create_screen();
    msg_create();
    screens_register(screens, sizeof(screens) / sizeof(screens[0]));
    set_text(m_title, "ColorMesh");
    set_text(m_body, tr(T_STARTING));
    lv_screen_load(scr_msg);
    slide_set_drag_hook(drag_started, NULL);
    lv_timer_create(fast_tick, 50, NULL);
    lv_timer_create(slow_tick, 1000, NULL);
    display_unlock();
}

void ui_show_pick(void)
{
    display_lock(-1);
    show_pick_locked();
    display_unlock();
}

void ui_show_main(void)
{
    display_lock(-1);
    show_main_locked();
    display_unlock();
}

void ui_message(const char *title, const char *body)
{
    display_lock(-1);
    set_text(m_title, title);
    set_text(m_body, body);
    show_msg_locked();
    display_unlock();
}

void ui_texts_changed(void)
{
    display_lock(-1);
    pick_refresh();
    main_update(true);
    system_refresh();
    display_unlock();
}
