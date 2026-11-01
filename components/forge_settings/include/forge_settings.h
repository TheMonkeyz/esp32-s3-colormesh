#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

// The Settings screen an espforge app opens with a long press (weather_amoled's, made shared, October 2026).
//
// A Done button at the top, then rows in sections that scroll, and optionally a brightness arc along the bottom edge
// (the finger's x on the band below it sets it, previewed live, saved when the finger lifts). It slides up from below;
// Done or a swipe right slides it back down to the screen it came from. While it is shown it refreshes every second, so
// a change made from the phone's settings page appears on it.
//
// Rows: a section title, a switch (the whole row toggles it), a value (a tap cycles it), an action (a tap does it; its
// value says what it will do or how it went), an info line (not tappable). Each is a struct of callbacks; the
// ready-made rows (settings_std_*) cover screen dimming (forge_presence), the language, the phone's settings page (a
// QR code), Wi-Fi, updates (forge_ota), restart and an About section: what weather_amoled and esp32-s3-rtcquebec show.
// The app adds rows of its own anywhere in the list.
//
// Texts: the app's. Each row's label is one of the app's text ids (i18n_strings.h, read with i18n_text); the
// ready-made rows' words come from settings_opts_t.texts, the app's id for each settings_text_t code. Components
// never show text of their own.

typedef enum {
    SET_T_DONE,             // "Done"
    SET_T_SEC_SCREEN,       // "Screen"
    SET_T_DIM_QUIET,        // "Dim when quiet"
    SET_T_WAKE_PICKUP,      // "Wake on pick-up"
    SET_T_TIMING,           // "Timing"
    SET_T_SHORT,            // "Short"
    SET_T_NORMAL,           // "Normal"
    SET_T_LONG,             // "Long"
    SET_T_CUSTOM,           // "Custom"
    SET_T_BRIGHTNESS,       // "Brightness %d%%"
    SET_T_LANGUAGE,         // "Language"
    SET_T_SEC_MORE,         // "More"
    SET_T_PHONE,            // "Phone settings"
    SET_T_PHONE_SCAN,       // "Scan with your phone's camera to open the settings page"
    SET_T_PHONE_NONE,       // "Connect to Wi-Fi first"
    SET_T_WIFI,             // "Wi-Fi network"
    SET_T_UPDATES,          // "Updates"
    SET_T_CHECK_NOW,        // "Check now"
    SET_T_CHECKING,         // "Checking…"
    SET_T_UP_TO_DATE,       // "Up to date"
    SET_T_FAILED,           // "Failed"
    SET_T_INSTALL,          // "Install %s"
    SET_T_TAP_AGAIN,        // "Tap again"
    SET_T_RESTART,          // "Restart"
    SET_T_RESTARTING,       // "Restarting…"
    SET_T_SEC_ABOUT,        // "About"
    SET_T_VERSION,          // "Version"
    SET_T_NETWORK,          // "Network"
    SET_T_OFFLINE,          // "Offline"
    SET_T_IP,               // "IP address"
    SET_T_MEMORY,           // "Memory"
    SET_T_MEMORY_KB,        // "%u / %u KB" (internal RAM / PSRAM free)
    SET_T_UPTIME,           // "Running for"
    SET_T_UPTIME_MIN,       // "%d min"
    SET_T_UPTIME_H,         // "%d h %02d"
    SET_T_UPTIME_D,         // "%d d %d h"
    SET_T_COUNT
} settings_text_t;

typedef enum { SET_SECTION, SET_SWITCH, SET_VALUE, SET_ACTION, SET_INFO } settings_kind_t;

// One row. Every callback gets the row's `user`; all run in the LVGL task with the display lock held. NULL = none.
typedef struct {
    settings_kind_t kind;
    int label;                                     // the app's text id (i18n_text); ready-made rows: -1 - code
    bool (*on)(void *user);                        // SWITCH: its state
    void (*value)(void *user, char *out, size_t n);   // VALUE / ACTION / INFO: the text on the right ("" = none)
    void (*tap)(void *user);                       // SWITCH / VALUE / ACTION: the row was tapped (then it refreshes)
    bool (*shown)(void *user);                     // NULL = always shown
    void *user;
} settings_row_t;

typedef struct {
    const int *texts;                              // [SET_T_COUNT]: the app's text id for each code
    const lv_font_t *font;                         // rows and Done (~20 px)
    const lv_font_t *font_small;                   // section titles, brightness, the QR page's note (~14 px)
    bool brightness;                               // the arc along the bottom (forge_presence's bright_pct)
    void (*closed)(void);                          // optional: Settings closed (Done, swipe, settings_close)
} settings_opts_t;

// Builds the screen (once, at start-up, display lock held). rows: copied. At most SETTINGS_ROWS_MAX rows.
#define SETTINGS_ROWS_MAX 32
lv_obj_t *settings_create(const settings_opts_t *opts, const settings_row_t *rows, int n);
void settings_open(void);                          // from the screen shown now (back to it on close)
void settings_close(void);
bool settings_shown(void);
void settings_refresh(void);                       // now (changes made elsewhere also appear within a second)
lv_obj_t *settings_screen(void);
// For snapshots (forge.json "settings1"...): the list scrolled down by `page` heights of its box (0 = top), then
// (show_page) the screen loaded without a slide
void settings_scroll(int page);
void settings_show_page(int page);

// The page an action row opened (the app's Wi-Fi setup) closed: back to Settings if it was opened from there. True if
// so (the app then loads nothing else).
bool settings_resume(void);
// An action row of the app's that opens a screen of its own: Settings remembers to come back (settings_resume)
void settings_leave_for(void);

// ---- ready-made rows ----
settings_row_t settings_std_section(settings_text_t title);
// Screen dimming (forge_presence): on/off, wake on pick-up (shown only with a motion sensor), timing Short / Normal /
// Long (Custom when set otherwise from the page)
settings_row_t settings_std_dim(void);
settings_row_t settings_std_motion(void);
settings_row_t settings_std_timing(void);
// The language: a tap goes to the next of the app's languages, saved (i18n_save); changed() then redraws the app
settings_row_t settings_std_language(void (*changed)(void));
// The settings page on the phone: a QR code of web_url() (with the key) on a page of its own; a tap goes back
settings_row_t settings_std_phone(void);
// Wi-Fi: open() is the app's setup screen; it calls settings_resume() when it closes
settings_row_t settings_std_wifi(void (*open)(void));
// Updates (forge_ota): "Check now"; checking, up to date, failed for a few seconds after; an update offered: "Install
// vX.Y.Z", installed on a second tap within 4 s
settings_row_t settings_std_updates(void);
// Restart on a second tap within 4 s (ota_restart_when_safe: not in an update's first minute)
settings_row_t settings_std_restart(void);
// About: version, network (name and signal), IP address, memory (internal / PSRAM free, KB), uptime
settings_row_t settings_std_version(void);
settings_row_t settings_std_network(void);
settings_row_t settings_std_ip(void);
settings_row_t settings_std_memory(void);
settings_row_t settings_std_uptime(void);

// The internals the ready-made rows use (settings.c)
const char *settings_text(settings_text_t code);
void settings_show_phone(void);

// How the Settings screen (and its phone page) slide in and out: forge_lvgl's slide_to by default. An app with its own
// slide.c (weather_amoled) gives its own: fn(scr, dir, vertical), dir 1 = from the right / below. NULL = the default.
void settings_set_slider(void (*fn)(lv_obj_t *scr, int dir, bool vertical));
