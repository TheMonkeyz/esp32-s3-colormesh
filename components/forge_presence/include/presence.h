#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Screen dimming by presence (weather_amoled's presence.c, through esp32-s3-rtcquebec v0.3.0): sound, movement and
// the touch screen tell whether someone is around; the screen dims, then turns off, when the room stays quiet.
//   ACTIVE --(quiet for dim_s)--> DIM --(quiet for off_s more)--> OFF
//   DIM/OFF --(noise sustained for wake_s)--> ACTIVE      (a single bang doesn't wake it)
//   A touch or a movement wakes it, and counts as activity while it is on.
// The hardware comes in through hooks (presence_hooks_t): the component doesn't know the board, so an app with its
// own board code (weather_amoled) uses it too. docs/COMPONENTS.md "forge_presence".

typedef enum { PRESENCE_ACTIVE = 0, PRESENCE_DIM = 1, PRESENCE_OFF = 2 } presence_state_t;

typedef struct {
    bool  enabled;
    float margin_db;      // noise must exceed baseline + margin to count as "loud"
    float wake_s;         // seconds of (mostly) continuous noise needed to wake from DIM/OFF
    float dim_s;          // quiet seconds before dimming
    float off_s;          // further quiet seconds before the screen turns off
    int   bright_pct;     // normal brightness
    int   dim_pct;        // dimmed brightness
    float baseline_db;    // background noise (dBFS), set by calibration
} presence_cfg_t;

typedef enum { PRESENCE_CAL_NONE = 0, PRESENCE_CAL_OK, PRESENCE_CAL_NOISY } presence_cal_t;

typedef struct {
    float level_db, threshold_db, wake_progress, quiet_s, calib_left_s;
    presence_state_t state;
    bool  calibrating, mic_ok;
    int   brightness;
    bool  imu_ok;         // motion sensor found
    float motion_g;       // recent movement (change from the resting position, g; peak, decays in ~1 s)
    float motion_thr;     // movement that wakes it (g)
    presence_cal_t last_cal;   // the last calibration's verdict (NOISY: the baseline was kept)
    float cal_spread_db;       // its spread (90th - 10th percentile)
} presence_status_t;

// What the app supplies. Every hook may be NULL: no microphones = the state machine doesn't run (always ACTIVE),
// no motion sensor = no pick-up option. Called from the presence task (core 0).
typedef struct {
    // Microphones: called once at start (true = they work), then one read per 100 ms window of 16 kHz samples
    // (n int16 samples, any channel layout: only their loudness counts); false = a failed read (counted, logged).
    bool (*mic_open)(void);
    bool (*mic_read)(int16_t *samples, size_t n);
    // Motion sensor: once at start, then each window: acceleration in g
    bool (*accel_open)(void);
    bool (*accel_read)(float g[3]);
    // Milliseconds since a finger was last down (the board's touch_idle_ms); NULL = only presence_touch() counts
    uint32_t (*touch_idle_ms)(void);
    // Sets the screen's brightness (0..100 %), taking the display lock itself
    void (*set_brightness)(int pct);
    // Optional (NULL: brightness 0 is "off"): a real off. Called once the fade down has reached 0 with the state OFF
    // (off = true), and before the fade up starts (off = false): the board sleeps and wakes its panel there, and the
    // CPU may idle slower meanwhile (the starter's main.c gives it board.h's display_sleep). The brightness slider's
    // preview never reaches 0, so a finger on it can't put the panel to sleep.
    void (*screen_off)(bool off);
    // The settings changed through the page (POST /api/presence): an app that shows them on a screen redraws it
    void (*settings_changed)(void);
} presence_hooks_t;

void presence_start(const presence_hooks_t *hooks);   // loads the settings, starts the task, the console commands
void presence_web_routes(void);                     // GET/POST /api/presence, POST /api/calibrate; before web_start()

void presence_get_config(presence_cfg_t *out);
bool presence_set_config(const presence_cfg_t *in); // saves to NVS (baseline is kept); false = not saved
void presence_get_status(presence_status_t *st);
bool presence_calibrate(int seconds);               // measure background noise; keep quiet meanwhile
void presence_wake(void);
// A brightness slider being dragged: shown at once (no fade), kept as the full level without saving; the fade takes
// over again 300 ms after the last call. Save on release with presence_set_config().
void presence_preview_brightness(int pct);
// A finger came down: wakes; true if the screen was off. Give it to the board as its press filter
// (touch_set_press_filter(presence_touch)): the touch that wakes a dark screen does nothing else.
bool presence_touch(void);
bool presence_screen_off(void);
bool presence_motion_wake(void);                    // wake on pick-up / movement (saved)
bool presence_set_motion(bool on, float threshold_g);   // threshold 0.02..0.5 g (saved); false = not saved

// ---- pure C, host-tested (tests/host/test_presence.c) ----

// The state machine: one step of dt seconds. running: enabled and the microphones work (otherwise ACTIVE).
typedef struct {
    presence_state_t state;
    float score;          // sustained-noise score (s): rises while loud, falls at half speed while quiet
    float quiet_s;        // quiet time so far (dim_s and more once dimmed)
    float dark_touch_s;   // seconds left in which the press filter still counts the finger as the touch that woke a
                          // dark screen: set by the task when its own poll saw the finger before the filter did
} presence_sm_t;
#define PRESENCE_DARK_TOUCH_S 0.6f   // the filter runs within ~0.2 s of the wake (LVGL reads touch every 100 ms asleep)
presence_state_t presence_sm_step(presence_sm_t *sm, const presence_cfg_t *c, bool running, bool loud, bool moved,
                                  bool touched, float dt);
void presence_clamp_cfg(presence_cfg_t *c);         // the limits every setting is held to (page, NVS)
// The press filter's verdict for a finger that just came down: true when the screen was off, or when the task's poll
// woke it from off less than PRESENCE_DARK_TOUCH_S ago (that touch does nothing else). Clears the grace.
bool presence_sm_take_dark_touch(presence_sm_t *sm);

// The background noise from a calibration's levels (dBFS, one per window; sorted in place): their median. NOISY
// when the 90th and 10th percentiles are more than PRESENCE_CAL_SPREAD_DB apart (someone spoke during it: the 90th
// percentile weather_amoled took then sat 30 dB above the room) or there are fewer than 10: *baseline is left alone.
#define PRESENCE_CAL_SPREAD_DB 12.0f
presence_cal_t presence_calib_baseline(float *levels, int n, float *baseline, float *spread);

// weather_amoled's settings before forge_presence: one 32-byte NVS blob "cfg" (namespace "presence"): enabled (bool,
// 3 bytes padding), margin_db, wake_s, dim_s, off_s (float), bright_pct, dim_pct (int32), baseline_db (float),
// little-endian. Decoded field by field and clamped; false for any other size. presence_start() imports it once
// into the typed keys (when they don't exist yet) and leaves the blob for a rollback to the old firmware.
bool presence_cfg_from_blob_v1(const void *blob, size_t n, presence_cfg_t *out);
