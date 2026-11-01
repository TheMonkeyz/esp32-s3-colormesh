// The starter app's screens (see ui.h). Small on purpose: it shows the framework's paths (pager, screen registry,
// snapshots, simulated touch, Wi-Fi setup, updates, two languages) and is the part a new project replaces.
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "board.h"
#include "pager.h"
#include "screens.h"
#include "slide.h"
#include "textfit.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "web.h"
#include "app_text.h"
#include "forge_settings.h"

static const char *TAG = "ui";

#define C_BG     lv_color_hex(0x000000)       // AMOLED: black pixels are off
#define C_TEXT   lv_color_hex(0xF2F4F7)
#define C_DIM    lv_color_hex(0x8B95A1)
#define C_ACCENT lv_color_hex(0x4DA3FF)

#ifdef EMU_BUILD                             // the browser emulator (web/emu): the font is an array, its end a pointer
extern const uint8_t ttf_start[];
extern const uint8_t *const ttf_end;
#else
extern const uint8_t ttf_start[] asm("_binary_montserrat_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_montserrat_ttf_end");
#endif

static lv_font_t *f_big, *f_mid, *f_small, *f_tiny;
static lv_obj_t *scr_main, *pager, *scr_msg, *scr_setup;
static lv_obj_t *h_title, *h_clock, *h_date, *h_sub, *h_hint;
static lv_obj_t *s_title, *s_lines, *s_qr, *s_scan;
static lv_obj_t *m_title, *m_body;
static ota_status_t ota_st;              // last forge_ota status (copied under the display lock)

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

// Labels are never clickable: a decorative object that is clickable swallows presses (long-press never fired)
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

// Set only when it changes: a redraw that changes nothing still costs a frame (and invalidates cached pictures)
static void set_text(lv_obj_t *l, const char *s)
{
    if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

static lv_obj_t *make_qr(lv_obj_t *parent, int size)
{
    lv_obj_t *qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_remove_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    return qr;
}

// A long press: Settings (forge_settings); offline, Wi-Fi setup is what's needed (as weather_amoled)
static void long_pressed(lv_event_t *e)
{
    if (!slide_held_still(SLIDE_HOLD_PX)) { ESP_LOGI(TAG, "long-press from a moving finger: a swipe, ignored (L200)"); return; }
    if (net_is_connected()) {
        ESP_LOGI(TAG, "long-press: settings");
        settings_open();
        return;
    }
    ESP_LOGI(TAG, "long-press while offline: Wi-Fi setup");
    ui_wifi_setup(NULL);
}

static void msg_long_pressed(lv_event_t *e)          // the start-up message: Wi-Fi setup
{
    if (!slide_held_still(SLIDE_HOLD_PX)) { ESP_LOGI(TAG, "long-press from a moving finger: a swipe, ignored (L200)"); return; }
    ESP_LOGI(TAG, "long-press: Wi-Fi setup");
    ui_wifi_setup(NULL);
}

/* ---------- hello and system pages ---------- */

static void fmt_uptime(char *out, int n)
{
    int64_t s = esp_timer_get_time() / 1000000;
    if (s < 3600) snprintf(out, n, "%d min", (int)(s / 60));
    else if (s < 86400) snprintf(out, n, "%d h %02d", (int)(s / 3600), (int)(s / 60 % 60));
    else snprintf(out, n, "%d d %d h", (int)(s / 86400), (int)(s / 3600 % 24));
}

static void update_line(char *out, int n)
{
    switch (ota_st.state) {
    case OTA_CHECKING:    snprintf(out, n, "%s", tr(T_UPD_CHECKING)); break;
    case OTA_UP_TO_DATE:  snprintf(out, n, "%s", tr(T_UPD_UP_TO_DATE)); break;
    case OTA_AVAILABLE:   snprintf(out, n, tr(T_UPD_AVAILABLE), ota_st.latest); break;
    case OTA_DOWNLOADING: snprintf(out, n, tr(T_UPD_DOWNLOADING), ota_st.progress); break;
    case OTA_DONE:        snprintf(out, n, "%s", tr(T_UPD_DONE)); break;
    case OTA_FAILED: {
        static const tid_t why[] = { [OTA_E_NO_SITE] = T_OTA_NO_SITE, [OTA_E_BAD_SITE] = T_OTA_BAD_SITE,
            [OTA_E_NO_IMAGE] = T_OTA_NO_IMAGE, [OTA_E_NO_START] = T_OTA_NO_START, [OTA_E_WRONG] = T_OTA_WRONG,
            [OTA_E_INTERRUPTED] = T_OTA_INTERRUPTED, [OTA_E_INVALID] = T_OTA_INVALID };
        int e = ota_st.err;
        snprintf(out, n, tr(T_UPD_FAILED), e > 0 && e <= OTA_E_INVALID ? tr(why[e]) : ota_st.error);
        break;
    }
    default:              snprintf(out, n, "%s", tr(T_UPD_IDLE));
    }
}

static void hello_refresh(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[64];
    if (tm.tm_year > 120) {                               // the clock is set (SNTP)
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        set_text(h_clock, buf);
        tr_date_long(&tm, buf, sizeof(buf));
        set_text(h_date, buf);
    } else {
        set_text(h_clock, "--:--");
        set_text(h_date, "");
    }
    set_text(h_title, tr(T_HELLO));
    set_text(h_sub, tr(T_HELLO_SUB));
    set_text(h_hint, tr(T_SWIPE_HINT));
}

static void system_refresh(void)
{
    char lines[400], up[24], upd[96], ip[20] = "-", ssid[NET_SSID_MAX + 1] = "", wifi[80];
    wifi_ap_record_t ap;
    char name[NET_SSID_MAX + 1];
    if (net_is_connected() && net_get_ssid(ssid, sizeof(ssid)) && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        textfit(ssid, name, sizeof(name));                // a network name may hold emoji: the font has none
        snprintf(wifi, sizeof(wifi), tr(T_SYS_WIFI), name, ap.rssi);
    }
    else
        snprintf(wifi, sizeof(wifi), "%s", tr(T_SYS_OFFLINE));
    net_get_ip(ip, sizeof(ip));
    fmt_uptime(up, sizeof(up));
    update_line(upd, sizeof(upd));
    int n = snprintf(lines, sizeof(lines), tr(T_SYS_VERSION), esp_app_get_description()->version);
    n += snprintf(lines + n, sizeof(lines) - n, "\n%s\n", wifi);
    n += snprintf(lines + n, sizeof(lines) - n, tr(T_SYS_IP), ip);
    n += snprintf(lines + n, sizeof(lines) - n, "\n");
    n += snprintf(lines + n, sizeof(lines) - n, tr(T_SYS_MEMORY),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    n += snprintf(lines + n, sizeof(lines) - n, "\n");
    n += snprintf(lines + n, sizeof(lines) - n, tr(T_SYS_UPTIME), up);
    snprintf(lines + n, sizeof(lines) - n, "\n%s", upd);
    set_text(s_title, tr(T_SYSTEM));
    set_text(s_lines, lines);
    set_text(s_scan, tr(T_SYS_SCAN));
    static char qr_text[80];                              // the settings page with the key (web.c)
    char url[80];
    if (web_url(url, sizeof(url))) {
        if (strcmp(url, qr_text)) {
            strlcpy(qr_text, url, sizeof(qr_text));
            lv_qrcode_update(s_qr, qr_text, strlen(qr_text));
        }
        lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_scan, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_scan, LV_OBJ_FLAG_HIDDEN);
    }
}

// Every second, both pages: the one not shown too, because a swipe shows a picture of it rendered in the background
// (slide.c, refreshed every 2 s): with its texts updated only when shown, a swipe showed stale ones for a moment
// ("Wi-Fi offline" after a restart). Changing an off-screen label costs no redraw.
static void tick(lv_timer_t *t)
{
    if (lv_screen_active() != scr_main) return;
    hello_refresh();
    system_refresh();
}

static void page_settled(int page, void *user)
{
    ESP_LOGI(TAG, "page %s", page ? "system" : "hello");
    if (page == 1) system_refresh();
    else hello_refresh();
}

static void main_create(void)
{
    scr_main = base_screen();
    pager = pager_create(scr_main, false, 2, NULL, page_settled, NULL);
    lv_obj_add_event_cb(pager, long_pressed, LV_EVENT_LONG_PRESSED, NULL);
    slide_pager(pager);                                   // drags drawn as pictures (~60 fps), not LVGL scrolling
    lv_obj_t *p0 = pager_page(pager, 0), *p1 = pager_page(pager, 1);
    h_title = label(p0, f_mid, C_ACCENT, 70, 300);
    h_clock = label(p0, f_big, C_TEXT, 130, 360);
    h_date = label(p0, f_small, C_TEXT, 215, 360);
    h_sub = label(p0, f_small, C_DIM, 265, 340);
    h_hint = label(p0, f_small, C_DIM, 330, 300);
    s_title = label(p1, f_mid, C_ACCENT, 40, 300);
    s_lines = label(p1, f_small, C_TEXT, 84, 380);
    s_qr = make_qr(p1, 92);
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, 262);
    s_scan = label(p1, f_small, C_DIM, 372, 300);   // y 372 + 2 lines: still inside the circle
    hello_refresh();
    system_refresh();
    lv_timer_create(tick, 1000, NULL);
}

/* ---------- message screen (start-up) ---------- */

static void msg_create(void)
{
    scr_msg = base_screen();
    m_title = label(scr_msg, f_mid, C_ACCENT, 120, 320);
    m_body = label(scr_msg, f_small, C_TEXT, 180, 340);
    lv_obj_add_flag(scr_msg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_msg, msg_long_pressed, LV_EVENT_LONG_PRESSED, NULL);
}

void ui_message(const char *title, const char *body)
{
    static char fitted[200];                              // network names in it may hold emoji (the font has none)
    textfit(body, fitted, sizeof(fitted));
    display_lock(-1);
    set_text(m_title, title);
    set_text(m_body, fitted);
    if (lv_screen_active() != scr_msg && lv_screen_active() != scr_setup) lv_screen_load(scr_msg);
    display_unlock();
}

void ui_home(void)
{
    display_lock(-1);
    hello_refresh();
    pager_go(pager, 0, false);
    if (lv_screen_active() != scr_main) lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    display_unlock();
}

/* ---------- Wi-Fi setup ----------
 * Page 1: the setup network (this device's own access point and captive portal): scan to join, the settings page
 * opens by itself. Page 2 (Android 10+): Wi-Fi Easy Connect (DPP). The phone scans this QR code and sends the
 * network it's connected to, password included. The setup network stays up on page 2: it holds the radio on Easy
 * Connect's channel (forge_net's dpp_hold_channel). The two pages are a pager like hello | system, so they follow the finger
 * (slide.c); each page has all its own objects (title, note, QR code, text, dots): a drag's picture of the page coming
 * in holds only that page. */

typedef struct { lv_obj_t *title, *note, *qr, *body; } su_page_t;
static su_page_t su[2];
static lv_obj_t *su_pager;
static int su_page;
static bool su_can_close;           // a tap closes it (not in first-time setup: there is no saved network)
static volatile bool su_open;
static lv_timer_t *su_timer;
static char su_note_text[96];
static char su_ap_qr[96];           // "WIFI:T:WPA;S:<setup SSID>;P:<this device's password>;;"

/* Easy Connect's code exists ~2 s after the page settles (a channel scan first, LESSONS L166). Until then the page
 * shows a placeholder code of the same size and density, faint and low-contrast ("loading"), so nothing pops in and a
 * drag's picture of the page already has it; the real code replaces it and fades up (the user found the pop-in janky).
 * LVGL 9.2 has no blur filter: low opacity and grey modules stand in for it. */
#define QR_FAINT LV_OPA_30
// As long as a real DPP URI (~100 characters): the same module count. Plain text, not a DPP URI: a phone that scans
// the faint placeholder gets a harmless message, not a broken Easy Connect link.
static const char QR_PLACEHOLDER[] =
    "Wait a moment: the Easy Connect code is being made. Scan again once it is bright, not faint........";
static bool qr_real;                                 // su[1].qr holds the real code

static void qr_placeholder(void)
{
    lv_qrcode_set_dark_color(su[1].qr, lv_color_hex(0x606060));
    lv_qrcode_update(su[1].qr, QR_PLACEHOLDER, strlen(QR_PLACEHOLDER));
    lv_obj_set_style_opa(su[1].qr, QR_FAINT, 0);
    lv_obj_remove_flag(su[1].qr, LV_OBJ_FLAG_HIDDEN);
    qr_real = false;
}

static void qr_opa(void *obj, int32_t v) { lv_obj_set_style_opa(obj, v, 0); }

// Easy Connect callbacks (system event task: take the display lock)
static void su_dpp_uri(const char *uri)
{
    display_lock(-1);
    lv_qrcode_set_dark_color(su[1].qr, lv_color_black());
    lv_qrcode_update(su[1].qr, uri, strlen(uri));
    lv_obj_remove_flag(su[1].qr, LV_OBJ_FLAG_HIDDEN);
    qr_real = true;
    lv_anim_t a;                                     // faint placeholder -> the real code (a 140 px square: cheap)
    lv_anim_init(&a);
    lv_anim_set_var(&a, su[1].qr);
    lv_anim_set_values(&a, QR_FAINT, LV_OPA_COVER);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, qr_opa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    display_unlock();
}

static void su_dpp_done(bool ok, const char *ssid)
{
    display_lock(-1);
    if (ok) {
        char name[NET_SSID_MAX + 1];
        textfit(ssid, name, sizeof(name));
        lv_label_set_text(su[1].title, tr(T_WIFI_RECEIVED));
        lv_label_set_text_fmt(su[1].body, tr(T_WIFI_GOT), name);
        lv_obj_add_flag(su[1].qr, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(su[1].body, tr(T_WIFI_DPP_FAIL));
    }
    display_unlock();
}

static void su_texts(void)                          // both pages' texts (snapshots use this without starting anything)
{
    lv_label_set_text(su[0].title, tr(T_WIFI_SETUP));
    snprintf(su_ap_qr, sizeof(su_ap_qr), "WIFI:T:WPA;S:" SETUP_AP_SSID ";P:%s;;", net_setup_ap_pass());
    lv_qrcode_update(su[0].qr, su_ap_qr, strlen(su_ap_qr));
    lv_label_set_text_fmt(su[0].body, tr(T_WIFI_JOIN), SETUP_AP_SSID, net_setup_ap_pass());
    lv_label_set_text(su[1].title, tr(T_WIFI_DPP_TITLE));
    if (!net_dpp_active() || !qr_real) qr_placeholder();   // until the code is generated
    lv_label_set_text(su[1].body, tr(T_WIFI_DPP_HOW));
    const char *note = su_note_text[0] ? su_note_text : !su_can_close ? "" :
                       net_is_connected() ? tr(T_TAP_CANCEL) : tr(T_TAP_RETRY);
    for (int i = 0; i < 2; i++) lv_label_set_text(su[i].note, note);
}

/* The radio work of a page (stopping the other mode, Easy Connect's channel scan: up to a few seconds) runs in its own
 * task: done in the swipe's handler it froze the screen, and the page change looked slow. Only the latest page request
 * counts; stops are never skipped (a stop queued by ui_wifi_setup_close() was replaced by a later request, and
 * main.c then stopped Easy Connect from its own task at the same time: two deinits). RADIO_DPP_OFF wakes
 * ui_wifi_setup_end() when done (weather_amoled v1.13.0). */
static QueueHandle_t su_q;
static SemaphoreHandle_t su_dpp_off;
enum { RADIO_OFF = -1, RADIO_AP = 0, RADIO_DPP = 1, RADIO_DPP_OFF = 2 };

static void su_radio_do(int mode);

static void su_radio_task(void *arg)
{
    int mode, next;
    while (xQueueReceive(su_q, &mode, portMAX_DELAY)) {
        while (xQueueReceive(su_q, &next, 0)) {
            if (mode != RADIO_AP && mode != RADIO_DPP) su_radio_do(mode);   // a stop: done, not replaced
            mode = next;
        }
        su_radio_do(mode);
    }
}

static void su_radio_do(int mode)
{
    if (mode == RADIO_AP) {
        net_dpp_stop();
        net_setup_ap_start();
    } else if (mode == RADIO_DPP) {
        // The setup network stays up: net_dpp_start keeps it on the Easy Connect channel (it holds the radio there)
        if (!net_dpp_start(su_dpp_uri, su_dpp_done)) {
            display_lock(-1);
            lv_label_set_text(su[1].body, tr(T_WIFI_DPP_NONE));
            lv_obj_add_flag(su[1].qr, LV_OBJ_FLAG_HIDDEN);   // no code is coming: no placeholder either
            display_unlock();
        }
    } else if (mode == RADIO_DPP_OFF) {
        net_dpp_stop();
        xSemaphoreGive(su_dpp_off);
    } else {
        net_dpp_stop();
        net_setup_ap_stop();
    }
}

static void su_radio(int mode) { if (su_q) xQueueSend(su_q, &mode, 0); }

static void su_show_page(int page)                  // the radio for that page (in its task)
{
    if (page == 0 && qr_real) qr_placeholder();      // Easy Connect stops: its next code will be a new one
    su_page = page;
    su_radio(page ? RADIO_DPP : RADIO_AP);
    ESP_LOGI(TAG, "Wi-Fi setup page %d (%s)", page, page ? "Easy Connect" : "setup network");
}

static void su_settled(int page, void *user) { if (su_open && page != su_page) su_show_page(page); }

// Leaving setup by any path stops what it started: the setup network pauses the saved network's retries, so one
// left open behind another screen kept the device offline (the harness's "screen hello" after "screen setup")
static void su_leave(void)
{
    if (!su_open) return;
    ESP_LOGI(TAG, "Wi-Fi setup closed");
    su_open = false;
    su_note_text[0] = 0;                // it was for that opening (and in that opening's language)
    if (su_timer) { lv_timer_delete(su_timer); su_timer = NULL; }
    su_radio(RADIO_OFF);
}

static void su_close(void)
{
    su_leave();
    if (settings_resume()) return;                   // opened from Settings' Wi-Fi row: back there
    lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

// Online: close after 10 min. Offline: close after 5 idle min so the saved network is tried again (setup pauses
// those attempts, and the router may just have been rebooting); main.c reopens setup if it still fails.
static void su_timeout(lv_timer_t *t)
{
    if (!su_can_close) return;                          // first-time setup stays
    if (!net_is_connected() && net_ap_clients() > 0) return;   // a phone is on the setup network
    su_close();
}

static void su_tap(lv_event_t *e)
{
    if (su_can_close) su_close();
}

static void setup_create(void)
{
    scr_setup = base_screen();
    lv_obj_add_flag(scr_setup, LV_OBJ_FLAG_CLICKABLE);
    su_pager = pager_create(scr_setup, false, 2, NULL, su_settled, NULL);
    // No Easy Connect on this board (Wi-Fi through a co-processor: forge_net's net_dpp_supported): the setup-network
    // page alone, the Easy Connect page hidden and out of the drags
    if (!net_dpp_supported()) pager_set_count(su_pager, 1);
    for (int p = 0; p < 2; p++) {
        lv_obj_t *pg = pager_page(su_pager, p);
        su[p].title = label(pg, f_mid, C_ACCENT, 40, 300);
        su[p].note = label(pg, f_small, C_DIM, 76, 330);   // up to 2 lines (T_CANT_REACH)
        su[p].qr = make_qr(pg, 140);
        lv_obj_align(su[p].qr, LV_ALIGN_TOP_MID, 0, 132);
        su[p].body = label(pg, f_small, C_TEXT, 292, 360);
        for (int i = 0; i < 2; i++) {                     // the dots, on each page: this one's is long
            lv_obj_t *d = lv_obj_create(pg);
            lv_obj_remove_style_all(d);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(d, 4, 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_set_size(d, i == p ? 18 : 7, 7);
            lv_obj_set_style_bg_color(d, i == p ? C_TEXT : C_DIM, 0);
            lv_obj_align(d, LV_ALIGN_BOTTOM_MID, i == 0 ? -10 : 10, -16);
        }
    }
    slide_pager(su_pager);                                 // drags follow the finger (pictures, ~60 fps)
    lv_obj_add_event_cb(scr_setup, su_tap, LV_EVENT_SHORT_CLICKED, NULL);
}

void ui_wifi_setup(const char *note)
{
    display_lock(-1);
    if (note) strlcpy(su_note_text, note, sizeof(su_note_text));
    else su_note_text[0] = 0;
    su_can_close = !net_in_portal();
    su_open = true;
    su_texts();
    pager_go(su_pager, 0, false);
    su_show_page(0);
    if (su_timer) lv_timer_delete(su_timer);
    su_timer = lv_timer_create(su_timeout, (net_is_connected() ? 10 : 5) * 60 * 1000, NULL);
    if (lv_screen_active() != scr_setup) lv_screen_load(scr_setup);
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);                              // the long-press isn't also a tap
    display_unlock();
}

bool ui_wifi_setup_open(void) { return su_open; }

bool ui_wifi_setup_close(void)
{
    display_lock(-1);
    bool close = su_open && su_can_close && net_ap_clients() == 0;
    if (close) su_close();
    display_unlock();
    return close;
}

void ui_wifi_setup_end(void)
{
    display_lock(-1);
    if (su_timer) { lv_timer_delete(su_timer); su_timer = NULL; }
    display_unlock();
    int mode = RADIO_DPP_OFF;                       // through the radio task: never at the same time as its own stop
    xSemaphoreTake(su_dpp_off, 0);
    if (su_q && xQueueSend(su_q, &mode, pdMS_TO_TICKS(1000)) == pdTRUE) xSemaphoreTake(su_dpp_off, pdMS_TO_TICKS(10000));
}

/* ---------- updates, language ---------- */

void ui_ota(const ota_status_t *st)
{
    display_lock(-1);
    ota_st = *st;
    if (lv_screen_active() == scr_main && pager_current(pager) == 1) system_refresh();
    display_unlock();
}

void ui_texts_changed(void)
{
    display_lock(-1);
    hello_refresh();
    system_refresh();
    if (su_open) su_texts();
    display_unlock();
}

/* ---------- Settings (forge_settings) ---------- */

// The app's words for forge_settings' codes, in settings_text_t's order
static const int set_texts[] = {
    T_SET_DONE, T_SET_SEC_SCREEN, T_SET_DIM_QUIET, T_SET_WAKE_PICKUP, T_SET_TIMING, T_SET_SHORT, T_SET_NORMAL,
    T_SET_LONG, T_SET_CUSTOM, T_SET_BRIGHTNESS, T_SET_LANGUAGE, T_SET_SEC_MORE, T_SET_PHONE, T_SET_PHONE_SCAN,
    T_SET_PHONE_NONE, T_SET_WIFI, T_SET_UPDATES, T_SET_CHECK_NOW, T_SET_CHECKING, T_SET_UP_TO_DATE, T_SET_FAILED,
    T_SET_INSTALL, T_SET_TAP_AGAIN, T_SET_RESTART, T_SET_RESTARTING, T_SET_SEC_ABOUT, T_SET_VERSION, T_SET_NETWORK,
    T_SET_OFFLINE, T_SET_IP, T_SET_MEMORY, T_SET_MEMORY_KB, T_SET_UPTIME, T_SET_UPTIME_MIN, T_SET_UPTIME_H,
    T_SET_UPTIME_D,
};
_Static_assert(sizeof(set_texts) / sizeof(set_texts[0]) == SET_T_COUNT, "one text per settings_text_t code");

static void wifi_from_settings(void) { ui_wifi_setup(NULL); }

static void lang_changed(void)                       // display lock held (a Settings row)
{
    hello_refresh();
    system_refresh();
}

static void settings_make(void)
{
    const settings_row_t rows[] = {
        settings_std_section(SET_T_SEC_SCREEN),
        settings_std_dim(), settings_std_motion(), settings_std_timing(),
        settings_std_language(lang_changed),
        settings_std_section(SET_T_SEC_MORE),
        settings_std_phone(), settings_std_wifi(wifi_from_settings), settings_std_updates(), settings_std_restart(),
        settings_std_section(SET_T_SEC_ABOUT),
        settings_std_version(), settings_std_network(), settings_std_ip(), settings_std_memory(), settings_std_uptime(),
    };
    settings_opts_t o = { .texts = set_texts, .font = f_small, .font_small = f_tiny, .brightness = true };
    settings_create(&o, rows, sizeof(rows) / sizeof(rows[0]));
}

/* ---------- screen registry (test console, snapshots) ---------- */

static lv_obj_t *get_hello(void) { return pager_page(pager, 0); }
static lv_obj_t *get_system(void) { return pager_page(pager, 1); }
static lv_obj_t *get_setup(void) { return pager_page(su_pager, 0); }
static lv_obj_t *get_setup1(void) { return pager_page(su_pager, 1); }
static lv_obj_t *get_msg(void) { return scr_msg; }
static void show_msg(void) { su_leave(); lv_screen_load(scr_msg); }   // (tests: a screen without a pager)
static void show_hello(void) { su_leave(); pager_go(pager, 0, false); lv_screen_load(scr_main); }
static void show_system(void) { su_leave(); system_refresh(); pager_go(pager, 1, false); lv_screen_load(scr_main); }
static void show_setup(void) { ui_wifi_setup(NULL); }
static bool shown_hello(void) { return lv_screen_active() == scr_main && pager_current(pager) == 0; }
static bool shown_system(void) { return lv_screen_active() == scr_main && pager_current(pager) == 1; }
static bool shown_setup(void) { return lv_screen_active() == scr_setup && pager_current(su_pager) == 0; }
static bool shown_setup1(void) { return lv_screen_active() == scr_setup && pager_current(su_pager) == 1; }
static void show_setup1(void) { ui_wifi_setup(NULL); pager_switch(su_pager, 1); }
static bool shown_msg(void) { return lv_screen_active() == scr_msg; }
static lv_obj_t *get_settings(void) { return settings_screen(); }
static void show_settings(void) { su_leave(); settings_show_page(0); }
static void show_settings1(void) { su_leave(); settings_show_page(1); }
static void prep_settings(void) { settings_scroll(0); }
static void prep_settings1(void) { settings_scroll(1); }
static void prep_settings2(void) { settings_scroll(2); }
static void show_settings2(void) { su_leave(); settings_show_page(2); }
static bool shown_settings(void) { return lv_screen_active() == settings_screen(); }
static bool shown_never(void) { return false; }      // settings1: the same screen, never named as the one shown
static void prep_setup(void)                         // texts only: no access point or Easy Connect is started
{
    if (su_open) return;
    su_can_close = !net_in_portal();
    su_texts();
}

static const screen_def_t screens[] = {
    { "hello",   get_hello,  show_hello,  hello_refresh,  shown_hello },
    { "system",  get_system, show_system, system_refresh, shown_system },
    { "setup",   get_setup,  show_setup,  prep_setup,     shown_setup },
    { "setup1",  get_setup1, show_setup1, prep_setup,     shown_setup1 },
    { "settings", get_settings, show_settings, prep_settings, shown_settings },
    // The list scrolled down a box height
    { "settings1", get_settings, show_settings1, prep_settings1, shown_never },
    { "settings2", get_settings, show_settings2, prep_settings2, shown_never },   // ...and further (About)
    { "message", get_msg,    show_msg,    NULL,           shown_msg },
};

void ui_init(void)
{
    textfit_init(ttf_start, ttf_end - ttf_start);
    su_q = xQueueCreate(4, sizeof(int));
    su_dpp_off = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(su_radio_task, "setup_radio", 4096, NULL, 3, NULL, 0);   // internal RAM: NVS writes
    display_lock(-1);
    f_big = mkfont(64);
    f_mid = mkfont(30);
    f_small = mkfont(20);
    f_tiny = mkfont(14);
    main_create();
    msg_create();
    setup_create();
    settings_make();
    screens_register(screens, sizeof(screens) / sizeof(screens[0]));
    lv_screen_load(scr_msg);
    display_unlock();
    web_set_snapshot(screens_snapshot, screens_snapshot_free);
}
