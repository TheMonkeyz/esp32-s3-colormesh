// Screen dimming by presence (presence.h): the task that reads the hooks every 100 ms, the settings in NVS, the console
// commands. The state machine and the calibration statistic are presence_sm.c (pure, host-tested).
#include "presence.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "testcon.h"
#include "presence_json.h"

static const char *TAG = "presence";

#define SAMPLE_RATE   16000
#define TICK_MS       100                        // analysis window
#define SAMPLES       (SAMPLE_RATE * TICK_MS / 1000 * 2)   // a window of 16 kHz stereo samples
#define CALIB_MAX     600                        // up to 60 s of 100 ms samples
#define MOTION_G      0.10f                      // default pick-up threshold: change from the resting position (g)
#define TOUCH_RECENT_MS (TICK_MS + 50)           // a finger down within this counts as this tick's activity

static presence_sm_t sm = { .state = PRESENCE_ACTIVE };   // the task steps it; the press filter asks it (dark_touch_s)

static presence_hooks_t hk;
static presence_cfg_t cfg = {
    .enabled = true, .margin_db = 10, .wake_s = 3, .dim_s = 600, .off_s = 3000,   // "Normal": dim 10 min, off at 60 min
    .bright_pct = 100, .dim_pct = 15, .baseline_db = -60,
};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static bool mic_ok, imu_ok;

static volatile presence_state_t state = PRESENCE_ACTIVE;
static volatile bool touch_woke;                          // presence_touch() since the task's last tick
static uint32_t mic_errors;                               // failed microphone reads since boot
static volatile float level_db = -90, score, quiet_s;
static volatile int cur_pct = -1;                 // brightness actually applied
static volatile bool calibrating;
static volatile int calib_left_ticks;
static float *calib_buf;
static int calib_n;
static volatile presence_cal_t last_cal;
static volatile float cal_spread;
static volatile bool motion_wake = true;
static volatile float motion_thr = MOTION_G;
static volatile float motion_g, motion_show;      // now; recent peak for the settings page meter

/* ---------------- settings ----------------
 * NVS namespace "presence", one typed key per setting: weather_amoled's "cfg" blob was dropped as unreadable when
 * the struct changed size, i.e. on an update. A missing key keeps its default.
 *   enabled u8 (0/1) | margin u16 (0.1 dB) | wake u16 (0.1 s) | dim u32 (s) | off u32 (s) | bright u8 (%)
 *   dim_pct u8 (%) | baseline i16 (0.1 dBFS) | motion u8 (0/1) | motion_mg u16 (mg, 20..500) */

static bool save_cfg(void);

// weather_amoled's "cfg" blob (presence_cfg_from_blob_v1) into the typed keys, once: `enabled` is written last, so a
// power cut midway leaves it missing and the next start imports again. The blob stays (a rollback reads it).
static void import_blob_v1(void)
{
    nvs_handle_t h;
    if (nvs_open("presence", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8, blob[32];
    size_t n = sizeof(blob);
    bool typed = nvs_get_u8(h, "enabled", &u8) == ESP_OK;
    esp_err_t e = typed ? ESP_ERR_NVS_NOT_FOUND : nvs_get_blob(h, "cfg", blob, &n);
    nvs_close(h);
    presence_cfg_t c;
    if (e != ESP_OK || !presence_cfg_from_blob_v1(blob, n, &c)) return;
    cfg = c;
    ESP_LOGI(TAG, "settings imported from the old \"cfg\" blob: %s", save_cfg() ? "saved" : "NOT saved");
}

static void load_cfg(void)
{
    import_blob_v1();
    nvs_handle_t h;
    if (nvs_open("presence", NVS_READONLY, &h) != ESP_OK) return;
    presence_cfg_t c = cfg;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    int16_t i16;
    if (nvs_get_u8(h, "enabled", &u8) == ESP_OK) c.enabled = u8;
    if (nvs_get_u16(h, "margin", &u16) == ESP_OK) c.margin_db = u16 / 10.0f;
    if (nvs_get_u16(h, "wake", &u16) == ESP_OK) c.wake_s = u16 / 10.0f;
    if (nvs_get_u32(h, "dim", &u32) == ESP_OK) c.dim_s = u32;
    if (nvs_get_u32(h, "off", &u32) == ESP_OK) c.off_s = u32;
    if (nvs_get_u8(h, "bright", &u8) == ESP_OK) c.bright_pct = u8;
    if (nvs_get_u8(h, "dim_pct", &u8) == ESP_OK) c.dim_pct = u8;
    if (nvs_get_i16(h, "baseline", &i16) == ESP_OK) c.baseline_db = i16 / 10.0f;
    presence_clamp_cfg(&c);
    cfg = c;
    if (nvs_get_u8(h, "motion", &u8) == ESP_OK) motion_wake = u8;
    if (nvs_get_u16(h, "motion_mg", &u16) == ESP_OK && u16 >= 20 && u16 <= 500) motion_thr = u16 / 1000.0f;
    nvs_close(h);
}

static bool nvs_ok(esp_err_t e, const char *what)
{
    if (e != ESP_OK) ESP_LOGE(TAG, "NVS %s: %s", what, esp_err_to_name(e));
    return e == ESP_OK;
}

static bool save_cfg(void)
{
    presence_cfg_t c;
    presence_get_config(&c);
    nvs_handle_t h;
    if (!nvs_ok(nvs_open("presence", NVS_READWRITE, &h), "open presence")) return false;
    bool ok = nvs_ok(nvs_set_u16(h, "margin", (uint16_t)lroundf(c.margin_db * 10)), "margin") &&
              nvs_ok(nvs_set_u16(h, "wake", (uint16_t)lroundf(c.wake_s * 10)), "wake") &&
              nvs_ok(nvs_set_u32(h, "dim", (uint32_t)lroundf(c.dim_s)), "dim") &&
              nvs_ok(nvs_set_u32(h, "off", (uint32_t)lroundf(c.off_s)), "off") &&
              nvs_ok(nvs_set_u8(h, "bright", (uint8_t)c.bright_pct), "bright") &&
              nvs_ok(nvs_set_u8(h, "dim_pct", (uint8_t)c.dim_pct), "dim_pct") &&
              nvs_ok(nvs_set_i16(h, "baseline", (int16_t)lroundf(c.baseline_db * 10)), "baseline") &&
              nvs_ok(nvs_set_u8(h, "motion", motion_wake), "motion") &&
              nvs_ok(nvs_set_u16(h, "motion_mg", (uint16_t)lroundf(motion_thr * 1000)), "motion_mg") &&
              nvs_ok(nvs_set_u8(h, "enabled", c.enabled), "enabled") &&      // last: the import's marker
              nvs_ok(nvs_commit(h), "commit");
    nvs_close(h);
    return ok;
}

void presence_get_config(presence_cfg_t *out)
{
    taskENTER_CRITICAL(&mux);
    *out = cfg;
    taskEXIT_CRITICAL(&mux);
}

bool presence_set_config(const presence_cfg_t *in)
{
    presence_cfg_t c = *in;
    c.baseline_db = cfg.baseline_db;              // only calibration changes the baseline
    presence_clamp_cfg(&c);
    taskENTER_CRITICAL(&mux);
    cfg = c;
    taskEXIT_CRITICAL(&mux);
    bool ok = save_cfg();
    presence_wake();                              // show the result of the new settings right away
    ESP_LOGI(TAG, "config: %s, margin %.0f dB, wake %.1f s, dim %.0f s, off +%.0f s, %d%%/%d%%",
             c.enabled ? "on" : "off", c.margin_db, c.wake_s, c.dim_s, c.off_s, c.bright_pct, c.dim_pct);
    return ok;
}

/* ---------------- status / control ---------------- */

void presence_get_status(presence_status_t *st)
{
    presence_cfg_t c;
    presence_get_config(&c);
    st->level_db = level_db;
    st->threshold_db = c.baseline_db + c.margin_db;
    st->state = state;
    st->wake_progress = c.wake_s > 0 ? score / c.wake_s : 0;
    st->quiet_s = quiet_s;
    st->calibrating = calibrating;
    st->calib_left_s = calib_left_ticks * TICK_MS / 1000.0f;
    st->mic_ok = mic_ok;
    st->brightness = cur_pct < 0 ? 0 : cur_pct;
    st->imu_ok = imu_ok;
    st->motion_g = motion_show;
    st->motion_thr = motion_thr;
    st->last_cal = last_cal;
    st->cal_spread_db = cal_spread;
}

bool presence_motion_wake(void) { return motion_wake; }

bool presence_set_motion(bool on, float threshold_g)
{
    if (!(threshold_g >= 0.02f)) threshold_g = 0.02f;
    if (threshold_g > 0.5f) threshold_g = 0.5f;
    motion_wake = on;
    motion_thr = threshold_g;
    ESP_LOGI(TAG, "wake on pick-up %s, threshold %.2f g", on ? "on" : "off", threshold_g);
    return save_cfg();
}

bool presence_calibrate(int seconds)
{
    if (!mic_ok || calibrating) return false;
    if (seconds < 2) seconds = 2;
    if (seconds > CALIB_MAX * TICK_MS / 1000) seconds = CALIB_MAX * TICK_MS / 1000;
    calib_n = 0;
    calib_left_ticks = seconds * 1000 / TICK_MS;
    calibrating = true;
    ESP_LOGI(TAG, "calibrating for %d s - keep quiet", seconds);
    return true;
}

static volatile TickType_t preview_until;          // presence_preview_brightness(): no fade until then

void presence_preview_brightness(int pct)
{
    pct = pct < 5 ? 5 : pct > 100 ? 100 : pct;
    taskENTER_CRITICAL(&mux);
    cfg.bright_pct = pct;
    taskEXIT_CRITICAL(&mux);
    preview_until = xTaskGetTickCount() + pdMS_TO_TICKS(300);
    presence_wake();
    if (hk.set_brightness) hk.set_brightness(pct);
    cur_pct = pct;
}

void presence_settings_changed(void)
{
    if (hk.settings_changed) hk.settings_changed();
}

void presence_wake(void)
{
    touch_woke = true;                            // the task's state machine wakes too (it owns the state)
    state = PRESENCE_ACTIVE;
    quiet_s = 0;
}

// The board's press filter (touch_set_press_filter), in the LVGL task, when a finger comes down: wakes, and says
// whether the screen was off, so that press is swallowed (no tap, no swipe, no long-press)
bool presence_touch(void)
{
    // The task's poll (touch_idle_ms) may have seen this finger first and woken the screen already: while the panel
    // sleeps LVGL reads touch only every 100 ms. The state machine remembers such a wake for a moment (L213).
    bool was_off = presence_sm_take_dark_touch(&sm);
    presence_wake();
    if (was_off) ESP_LOGI(TAG, "touch on a dark screen: wake (this touch does nothing else)");
    return was_off;
}

bool presence_screen_off(void) { return state == PRESENCE_OFF; }

/* ---------------- the task ---------------- */

static bool panel_off;                        // hk.screen_off(true) was called: the panel sleeps until the fade up

static void apply_brightness(int target)
{
    if (target == cur_pct || !hk.set_brightness) {
        // The fade reached 0 and the state is OFF (a slider never goes below 5): a real off, if the app has one
        if (target == 0 && cur_pct == 0 && state == PRESENCE_OFF && hk.screen_off && !panel_off) {
            hk.screen_off(true);
            panel_off = true;
        }
        return;
    }
    if (panel_off && target > 0) { hk.screen_off(false); panel_off = false; }   // the panel first, then the fade up
    // Fade: ~1 s from full to off
    int step = 10;
    int next = cur_pct < 0 ? target : (target > cur_pct ? (cur_pct + step > target ? target : cur_pct + step)
                                                          : (cur_pct - step < target ? target : cur_pct - step));
    hk.set_brightness(next);
    cur_pct = next;
}

static void calib_tick(void)
{
    if (calib_n < CALIB_MAX) calib_buf[calib_n++] = level_db;
    if (--calib_left_ticks > 0) return;
    presence_cfg_t c;
    presence_get_config(&c);
    float base = c.baseline_db, spread;
    presence_cal_t r = presence_calib_baseline(calib_buf, calib_n, &base, &spread);
    if (r == PRESENCE_CAL_OK) {
        taskENTER_CRITICAL(&mux);
        cfg.baseline_db = base;
        presence_clamp_cfg(&cfg);
        taskEXIT_CRITICAL(&mux);
        save_cfg();
    }
    cal_spread = spread;
    last_cal = r;
    calibrating = false;
    ESP_LOGI(TAG, "calibrated: %s, baseline %.1f dBFS (min %.1f, max %.1f, spread %.1f dB, %d samples)",
             r == PRESENCE_CAL_OK ? "ok" : "too noisy, baseline kept", r == PRESENCE_CAL_OK ? base : c.baseline_db,
             calib_n ? calib_buf[0] : 0, calib_n ? calib_buf[calib_n - 1] : 0, spread, calib_n);
}

static void presence_task(void *arg)
{
    int16_t *buf = heap_caps_malloc(SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    calib_buf = heap_caps_malloc(CALIB_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    mic_ok = buf && calib_buf && hk.mic_open && hk.mic_read && hk.mic_open();
    imu_ok = hk.accel_open && hk.accel_read && hk.accel_open();
    ESP_LOGI(TAG, "microphones %s, motion sensor %s, baseline %.1f dBFS", mic_ok ? "ready" : "NOT available",
             imu_ok ? "ready" : "none", cfg.baseline_db);
    const float dt = TICK_MS / 1000.0f;
    uint32_t log_tick = 0;
    presence_state_t last_state = sm.state;
    float rest[3] = {0}, motion_peak = 0;                     // resting acceleration, follows in ~2 s
    int imu_skip = 10;                                        // the first second of samples is junk (3.7 g seen)

    while (1) {
        bool heard = false;                                    // this tick's sound level is real
        if (mic_ok) {
            if (!hk.mic_read(buf, SAMPLES)) {
                // A failing read used to skip the rest of the loop (`continue`): brightness froze, with no log.
                // Count it, carry on as if quiet (motion and timers still work), log the first one.
                if (mic_errors++ == 0) ESP_LOGW(TAG, "microphone read failed (counted in the 5 s lines)");
                vTaskDelay(pdMS_TO_TICKS(TICK_MS));
            } else heard = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        }
        if (heard) {
            double acc = 0;
            for (int i = 0; i < SAMPLES; i++) acc += (double)buf[i] * buf[i];
            double rms = sqrt(acc / SAMPLES);
            level_db = rms > 0.5 ? 20.0f * log10f((float)(rms / 32768.0)) : -90.0f;
        }
        if (calibrating && heard) calib_tick();

        // Motion: distance from the resting position (a slow average), so tilting or lifting it counts and
        // lying still in any position doesn't
        bool moved = false;
        float a[3];
        if (imu_ok && hk.accel_read(a)) {
            if (imu_skip > 0) {
                if (--imu_skip == 0) for (int i = 0; i < 3; i++) rest[i] = a[i];
            } else {
                float d2 = 0;
                for (int i = 0; i < 3; i++) { float e = a[i] - rest[i]; d2 += e * e; rest[i] += e * 0.05f; }
                motion_g = sqrtf(d2);
                motion_show = motion_g > motion_show * 0.85f ? motion_g : motion_show * 0.85f;   // meter: peak, decays
                if (motion_g > motion_peak) motion_peak = motion_g;
                moved = motion_wake && motion_g > motion_thr;
            }
        }

        // A finger: presence_touch() (the press filter, at once) or one still down / just lifted (touch_idle_ms)
        bool poll_touch = hk.touch_idle_ms && hk.touch_idle_ms() < TOUCH_RECENT_MS;
        bool touched = touch_woke || poll_touch;
        if (poll_touch && !touch_woke && sm.state == PRESENCE_OFF)   // the poll saw the finger before the press filter
            sm.dark_touch_s = PRESENCE_DARK_TOUCH_S;                 // did: that touch must still only wake (L213)
        touch_woke = false;

        presence_cfg_t c;
        presence_get_config(&c);
        bool loud = heard && !calibrating && level_db > c.baseline_db + c.margin_db;
        if (moved && sm.state != PRESENCE_ACTIVE)
            ESP_LOGI(TAG, "picked up / moved (%.2f g): wake", motion_g);
        presence_sm_step(&sm, &c, c.enabled && mic_ok, loud, moved, touched, dt);
        score = sm.score;
        quiet_s = sm.quiet_s;
        state = sm.state;
        if (sm.state != last_state) {
            static const char *names[] = {"ACTIVE", "DIM", "OFF"};
            ESP_LOGI(TAG, "%s -> %s (level %.1f dB, threshold %.1f dB%s)", names[last_state], names[sm.state],
                     level_db, c.baseline_db + c.margin_db, touched ? ", touch" : "");
            last_state = sm.state;
        }
        int dim = c.dim_pct < c.bright_pct ? c.dim_pct : c.bright_pct;   // never brighter than "full"
        if ((int32_t)(xTaskGetTickCount() - preview_until) >= 0)   // a slider being dragged sets it itself
            apply_brightness(sm.state == PRESENCE_ACTIVE ? c.bright_pct : sm.state == PRESENCE_DIM ? dim : 0);

        if (++log_tick % 50 == 0 && mic_ok) {                  // every 5 s
            ESP_LOGI(TAG, "level %.1f dB (threshold %.1f), score %.1f/%.1f, quiet %.0f s, motion peak %.3f g%s",
                     level_db, c.baseline_db + c.margin_db, sm.score, c.wake_s, sm.quiet_s, motion_peak,
                     mic_errors ? ", MIC READ ERRORS" : "");
            if (mic_errors) ESP_LOGW(TAG, "%lu microphone read errors so far", (unsigned long)mic_errors);
            motion_peak = 0;
        }
    }
}

/* ---------------- console (docs/PROTOCOL.md §2) ---------------- */

static void cmd_presence(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "calibrate")) {
        if (presence_calibrate(atoi(argv[2]))) ESP_LOGI("test", "ok presence calibrate %s", argv[2]);
        else ESP_LOGI("test", "error presence calibrate: %s", mic_ok ? "busy" : "no microphones");
        return;
    }
    presence_status_t st;
    presence_get_status(&st);
    ESP_LOGI("test", "presence state=%d brightness=%d quiet_s=%.0f mic=%d imu=%d level=%.1f threshold=%.1f "
                     "calibrating=%d cal=%d spread=%.1f", st.state, st.brightness, st.quiet_s, st.mic_ok, st.imu_ok,
             st.level_db, st.threshold_db, st.calibrating, st.last_cal, st.cal_spread_db);
}

static void cmd_wake(int argc, char **argv)
{
    presence_wake();
    ESP_LOGI("test", "ok wake");
}

static void where_presence(char *out, size_t n)
{
    snprintf(out, n, " presence=%d", (int)state);
}

void presence_start(const presence_hooks_t *hooks)
{
    if (hooks) hk = *hooks;
    load_cfg();
    testcon_register("presence", "presence [calibrate SECONDS]", cmd_presence);
    testcon_register("wake", "wake", cmd_wake);
    testcon_add_where(where_presence);
    xTaskCreatePinnedToCore(presence_task, "presence", 4096, NULL, 2, NULL, 0);
}
