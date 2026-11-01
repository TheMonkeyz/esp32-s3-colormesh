// The ready-made rows (forge_settings.h): what weather_amoled's and esp32-s3-rtcquebec's Settings screens share.
#include "forge_settings.h"
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "forge_i18n.h"
#include "net.h"
#include "ota.h"
#include "presence.h"
#include "textfit.h"

static const char *TAG = "settings";
#define T(code) settings_text(code)
#define ARMED_MS 4000                                  // a second tap within this confirms (install, restart)
#define RESULT_MS 6000                                 // "Up to date" / "Failed" shown this long after "Check now"

// Labels of ready-made rows are codes (settings_text_t) until settings_create() turns them into the app's ids: stored
// as -1 - code (an app's ids are >= 0)
#define L(code) (-1 - (int)(code))

settings_row_t settings_std_section(settings_text_t title) { return (settings_row_t){ .kind = SET_SECTION, .label = L(title) }; }

/* ---------- screen dimming (forge_presence) ---------- */

static bool dim_on(void *u) { presence_cfg_t c; presence_get_config(&c); return c.enabled; }
static void dim_tap(void *u)
{
    presence_cfg_t c;
    presence_get_config(&c);
    c.enabled = !c.enabled;
    presence_set_config(&c);
}

static bool motion_on(void *u) { return presence_motion_wake(); }
static bool motion_shown(void *u) { presence_status_t st; presence_get_status(&st); return st.imu_ok; }
static void motion_tap(void *u)
{
    presence_status_t st;
    presence_get_status(&st);
    presence_set_motion(!presence_motion_wake(), st.motion_thr);
}

// As weather_amoled's page and screen (off = all the quiet before the screen goes off); Normal is forge_presence's
// default
static const struct { int dim, off, wake; settings_text_t name; } presets[] = {
    { 120, 900, 2, SET_T_SHORT }, { 600, 3600, 3, SET_T_NORMAL }, { 1800, 10800, 3, SET_T_LONG },
};
#define NPRESETS (int)(sizeof(presets) / sizeof(presets[0]))

static int preset_of(const presence_cfg_t *c)          // -1 = custom (set otherwise from the page)
{
    for (int i = 0; i < NPRESETS; i++)
        if ((int)c->dim_s == presets[i].dim && (int)(c->dim_s + c->off_s) == presets[i].off &&
            (int)c->wake_s == presets[i].wake) return i;
    return -1;
}

static void timing_value(void *u, char *out, size_t n)
{
    presence_cfg_t c;
    presence_get_config(&c);
    int p = preset_of(&c);
    snprintf(out, n, "%s", T(p < 0 ? SET_T_CUSTOM : presets[p].name));
}

static void timing_tap(void *u)
{
    presence_cfg_t c;
    presence_get_config(&c);
    int p = (preset_of(&c) + 1) % NPRESETS;              // custom -> Short
    c.dim_s = presets[p].dim;
    c.off_s = presets[p].off - presets[p].dim;
    c.wake_s = presets[p].wake;
    presence_set_config(&c);
}

/* ---------- language ---------- */

static void (*lang_changed)(void);
static void lang_value(void *u, char *out, size_t n) { snprintf(out, n, "%s", i18n_name(i18n_lang())); }
static void lang_tap(void *u)
{
    i18n_set((i18n_lang() + 1) % i18n_count());
    i18n_save();
    ESP_LOGI(TAG, "language %s", i18n_code(i18n_lang()));
    if (lang_changed) lang_changed();
}

/* ---------- more ---------- */

static void arrow(void *u, char *out, size_t n) { snprintf(out, n, ">"); }
static void phone_tap(void *u) { settings_show_phone(); }

static void (*wifi_open)(void);
static void wifi_tap(void *u)
{
    if (!wifi_open) return;
    settings_leave_for();
    wifi_open();
}

static uint32_t checked_at, install_armed, restart_armed;   // lv_tick of the tap; 0 = none

static bool recent(uint32_t at, uint32_t ms) { return at && lv_tick_elaps(at) < ms; }

static void updates_value(void *u, char *out, size_t n)
{
    ota_status_t o;
    ota_get_status(&o);
    bool just = recent(checked_at, RESULT_MS);
    if (o.state == OTA_AVAILABLE)
        recent(install_armed, ARMED_MS) ? snprintf(out, n, "%s", T(SET_T_TAP_AGAIN))
                                        : snprintf(out, n, T(SET_T_INSTALL), o.latest);
    else if (o.state == OTA_CHECKING) snprintf(out, n, "%s", T(SET_T_CHECKING));
    else if (o.state == OTA_DOWNLOADING) snprintf(out, n, "%d%%", o.progress);
    else if (just && o.state == OTA_UP_TO_DATE) snprintf(out, n, "%s", T(SET_T_UP_TO_DATE));
    else if (just && o.state == OTA_FAILED) snprintf(out, n, "%s", T(SET_T_FAILED));
    else snprintf(out, n, "%s", T(SET_T_CHECK_NOW));
}

static void updates_tap(void *u)
{
    ota_status_t o;
    ota_get_status(&o);
    if (o.state == OTA_CHECKING || o.state == OTA_DOWNLOADING) return;
    if (o.state == OTA_AVAILABLE) {
        if (!recent(install_armed, ARMED_MS)) { install_armed = lv_tick_get() | 1; return; }
        install_armed = 0;
        ESP_LOGI(TAG, "install %s from the settings screen", o.latest);
        ota_install();
        return;
    }
    ota_check_now();
    checked_at = lv_tick_get() | 1;
}

static bool restarting;

static void restart_value(void *u, char *out, size_t n)
{
    snprintf(out, n, "%s", restarting ? T(SET_T_RESTARTING) : recent(restart_armed, ARMED_MS) ? T(SET_T_TAP_AGAIN) : "");
}

static void do_restart(lv_timer_t *t) { lv_timer_delete(t); ota_restart_when_safe(); }   // (not in an update's first minute)

static void restart_tap(void *u)
{
    if (restarting) return;
    if (!recent(restart_armed, ARMED_MS)) { restart_armed = lv_tick_get() | 1; return; }   // (| 1: never 0 = none)
    ESP_LOGI(TAG, "restart from the settings screen");
    restarting = true;                                   // "Restarting..." (the row refreshes after this)
    lv_timer_create(do_restart, 400, NULL);
}

/* ---------- about ---------- */

static void version_value(void *u, char *out, size_t n) { snprintf(out, n, "%s", esp_app_get_description()->version); }

static void network_value(void *u, char *out, size_t n)
{
    char ssid[NET_SSID_MAX + 1], name[NET_SSID_MAX + 1];
    wifi_ap_record_t ap;
    if (net_is_connected() && net_get_ssid(ssid, sizeof(ssid)) && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        textfit(ssid, name, sizeof(name));                // a network name may hold emoji: the font has none
        snprintf(out, n, "%s  %d dBm", name, ap.rssi);
    } else snprintf(out, n, "%s", T(SET_T_OFFLINE));
}

static void ip_value(void *u, char *out, size_t n) { if (!net_get_ip(out, n)) snprintf(out, n, "-"); }

static void memory_value(void *u, char *out, size_t n)
{
    snprintf(out, n, T(SET_T_MEMORY_KB), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void uptime_value(void *u, char *out, size_t n)
{
    int64_t s = esp_timer_get_time() / 1000000;
    if (s < 3600) snprintf(out, n, T(SET_T_UPTIME_MIN), (int)(s / 60));
    else if (s < 86400) snprintf(out, n, T(SET_T_UPTIME_H), (int)(s / 3600), (int)(s / 60 % 60));
    else snprintf(out, n, T(SET_T_UPTIME_D), (int)(s / 86400), (int)(s / 3600 % 24));
}

/* ---------- the rows ---------- */

#define ROW(k, code, ...) (settings_row_t){ .kind = k, .label = L(code), __VA_ARGS__ }

settings_row_t settings_std_dim(void) { return ROW(SET_SWITCH, SET_T_DIM_QUIET, .on = dim_on, .tap = dim_tap); }
settings_row_t settings_std_motion(void)
{
    return ROW(SET_SWITCH, SET_T_WAKE_PICKUP, .on = motion_on, .tap = motion_tap, .shown = motion_shown);
}
settings_row_t settings_std_timing(void) { return ROW(SET_VALUE, SET_T_TIMING, .value = timing_value, .tap = timing_tap); }
settings_row_t settings_std_language(void (*changed)(void))
{
    lang_changed = changed;
    return ROW(SET_VALUE, SET_T_LANGUAGE, .value = lang_value, .tap = lang_tap);
}
settings_row_t settings_std_phone(void) { return ROW(SET_ACTION, SET_T_PHONE, .value = arrow, .tap = phone_tap); }
settings_row_t settings_std_wifi(void (*open)(void))
{
    wifi_open = open;
    return ROW(SET_ACTION, SET_T_WIFI, .value = arrow, .tap = wifi_tap);
}
settings_row_t settings_std_updates(void) { return ROW(SET_ACTION, SET_T_UPDATES, .value = updates_value, .tap = updates_tap); }
settings_row_t settings_std_restart(void) { return ROW(SET_ACTION, SET_T_RESTART, .value = restart_value, .tap = restart_tap); }
settings_row_t settings_std_version(void) { return ROW(SET_INFO, SET_T_VERSION, .value = version_value); }
settings_row_t settings_std_network(void) { return ROW(SET_INFO, SET_T_NETWORK, .value = network_value); }
settings_row_t settings_std_ip(void) { return ROW(SET_INFO, SET_T_IP, .value = ip_value); }
settings_row_t settings_std_memory(void) { return ROW(SET_INFO, SET_T_MEMORY, .value = memory_value); }
settings_row_t settings_std_uptime(void) { return ROW(SET_INFO, SET_T_UPTIME, .value = uptime_value); }
