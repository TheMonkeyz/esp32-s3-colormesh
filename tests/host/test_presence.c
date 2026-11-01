// forge_presence: the state machine (ACTIVE -> DIM -> OFF, what wakes it), the settings' limits, the calibration
// statistic and /api/presence's JSON (presence_sm.c, presence_json.c)
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "presence.h"
#include "presence_json.h"

#define DT 0.1f                                   // presence.c's tick (TICK_MS)

static const presence_cfg_t normal = {           // presence.c's defaults ("Normal")
    .enabled = true, .margin_db = 10, .wake_s = 3, .dim_s = 600, .off_s = 3000,
    .bright_pct = 100, .dim_pct = 15, .baseline_db = -60,
};

// n ticks of the same input; the state after them
static presence_state_t run(presence_sm_t *sm, int n, bool loud, bool moved, bool touched)
{
    presence_state_t s = sm->state;
    for (int i = 0; i < n; i++) s = presence_sm_step(sm, &normal, true, loud, moved, touched, DT);
    return s;
}
#define SECONDS(s) ((int)lroundf((s) / DT))

// Ticks until the state becomes `want` (loud or quiet all along), at most max
static int until(presence_sm_t *sm, presence_state_t want, bool loud, int max)
{
    int n = 0;
    while (sm->state != want && n < max) { presence_sm_step(sm, &normal, true, loud, false, false, DT); n++; }
    return n;
}

int main(void)
{
    // Quiet: dims after 10 min, off 50 min later (60 min in all). Ticks of 0.1 s summed in floats drift (~1 s an
    // hour, harmless): within 5 s is on time.
    presence_sm_t sm = { .state = PRESENCE_ACTIVE };
    int t = until(&sm, PRESENCE_DIM, false, SECONDS(4000));
    CHECK(abs(t - SECONDS(600)) <= SECONDS(5), "dim after %d ticks", t);
    t = until(&sm, PRESENCE_OFF, false, SECONDS(4000));
    CHECK(abs(t - SECONDS(3000)) <= SECONDS(5), "off %d ticks later", t);
    CHECK(run(&sm, SECONDS(3600), false, false, false) == PRESENCE_OFF, "stays off in silence");

    // A single bang (0.5 s loud) doesn't wake an off screen; sustained noise (3 s) does
    CHECK(run(&sm, 5, true, false, false) == PRESENCE_OFF, "a bang doesn't wake");
    run(&sm, SECONDS(10), false, false, false);
    CHECK(sm.score == 0, "the score falls back to 0: %f", sm.score);
    t = until(&sm, PRESENCE_ACTIVE, true, SECONDS(60));
    CHECK(abs(t - SECONDS(3)) <= 1, "3 s of noise wakes: %d ticks", t);
    CHECK(sm.quiet_s == 0, "quiet count restarts: %f", sm.quiet_s);

    // Mostly continuous noise wakes too: 1 s loud, 0.4 s quiet (falls at half speed) -> wakes within ~5 s
    sm = (presence_sm_t){ .state = PRESENCE_OFF };
    int ticks = 0;
    while (sm.state == PRESENCE_OFF && ticks < SECONDS(20)) {
        presence_sm_step(&sm, &normal, true, ticks % 14 < 10, false, false, DT);
        ticks++;
    }
    CHECK(sm.state == PRESENCE_ACTIVE && ticks < SECONDS(6), "intermittent noise woke after %d ticks", ticks);

    // While dim, a short noise restarts the off countdown (and doesn't wake)
    sm = (presence_sm_t){ .state = PRESENCE_DIM, .quiet_s = 600 + 2999 };
    CHECK(run(&sm, 2, true, false, false) == PRESENCE_DIM, "dim stays dim on a short noise");
    CHECK(fabsf(sm.quiet_s - 600) < 1e-3, "off countdown restarted: %f", sm.quiet_s);

    // Noise while active keeps it from dimming
    sm = (presence_sm_t){ .state = PRESENCE_ACTIVE, .quiet_s = 599 };
    run(&sm, 1, true, false, false);
    CHECK(sm.quiet_s == 0 && sm.state == PRESENCE_ACTIVE, "noise resets the quiet count");

    // Motion (pick-up) wakes at once, from dim or off, and counts as activity while on
    sm = (presence_sm_t){ .state = PRESENCE_OFF };
    CHECK(run(&sm, 1, false, true, false) == PRESENCE_ACTIVE, "motion wakes an off screen");
    sm = (presence_sm_t){ .state = PRESENCE_DIM, .quiet_s = 700 };
    CHECK(run(&sm, 1, false, true, false) == PRESENCE_ACTIVE, "motion wakes a dim screen");
    sm = (presence_sm_t){ .state = PRESENCE_ACTIVE, .quiet_s = 599 };
    CHECK(run(&sm, 1, false, true, false) == PRESENCE_ACTIVE && sm.quiet_s == 0, "motion is activity");

    // A touch: the same
    sm = (presence_sm_t){ .state = PRESENCE_OFF };
    CHECK(run(&sm, 1, false, false, true) == PRESENCE_ACTIVE, "touch wakes an off screen");
    sm = (presence_sm_t){ .state = PRESENCE_DIM, .quiet_s = 700 };
    CHECK(run(&sm, 1, false, false, true) == PRESENCE_ACTIVE, "touch wakes a dim screen");
    sm = (presence_sm_t){ .state = PRESENCE_ACTIVE, .quiet_s = 599 };
    CHECK(run(&sm, 1, false, false, true) == PRESENCE_ACTIVE && sm.quiet_s == 0, "touch is activity");

    // The press filter's verdict (presence_sm_take_dark_touch): a finger on an OFF screen is a dark-screen touch; so
    // is one the task's own poll saw first (it set dark_touch_s), for PRESENCE_DARK_TOUCH_S after that wake (L213)
    sm = (presence_sm_t){ .state = PRESENCE_OFF };
    CHECK(presence_sm_take_dark_touch(&sm) && sm.dark_touch_s == 0, "off: the touch only wakes");
    sm = (presence_sm_t){ .state = PRESENCE_ACTIVE };
    CHECK(!presence_sm_take_dark_touch(&sm), "lit: the touch acts");
    sm = (presence_sm_t){ .state = PRESENCE_ACTIVE, .dark_touch_s = PRESENCE_DARK_TOUCH_S };
    run(&sm, 3, false, false, true);                        // the filter runs 0.3 s after the poll's wake
    CHECK(presence_sm_take_dark_touch(&sm), "woken by the poll: the filter's late touch still only wakes");
    CHECK(!presence_sm_take_dark_touch(&sm), "taken once");
    sm = (presence_sm_t){ .state = PRESENCE_ACTIVE, .dark_touch_s = PRESENCE_DARK_TOUCH_S };
    run(&sm, SECONDS(1), false, false, false);
    CHECK(!presence_sm_take_dark_touch(&sm), "a second after the poll's wake: a new touch acts");

    // Turned off (or no microphone): always active, wherever it was
    sm = (presence_sm_t){ .state = PRESENCE_OFF, .quiet_s = 4000 };
    CHECK(presence_sm_step(&sm, &normal, false, false, false, false, DT) == PRESENCE_ACTIVE && sm.quiet_s == 0,
          "not running: active");

    // Limits: NaN and out-of-range values from the page or NVS
    presence_cfg_t c = { .margin_db = NAN, .wake_s = 0, .dim_s = -5, .off_s = 1e9f, .bright_pct = 0, .dim_pct = 200,
                         .baseline_db = NAN };
    presence_clamp_cfg(&c);
    CHECK(c.margin_db == 1 && fabsf(c.wake_s - 0.2f) < 1e-6 && c.dim_s == 1 && c.off_s == 86400, "%f %f %f %f",
          c.margin_db, c.wake_s, c.dim_s, c.off_s);
    CHECK(c.bright_pct == 5 && c.dim_pct == 100 && c.baseline_db == -100, "%d %d %f", c.bright_pct, c.dim_pct,
          c.baseline_db);
    presence_cfg_t d = normal;
    presence_clamp_cfg(&d);
    CHECK(d.dim_s == 600 && d.off_s == 3000 && d.baseline_db == -60 && d.margin_db == 10, "defaults kept");

    // Calibration: the median of a quiet room; a voice during it is refused, not taken as the room
    float lv[50], base = -60, spread;
    for (int i = 0; i < 50; i++) lv[i] = -67 + (i % 5) * 0.5f;                 // a steady room, -67..-65 dBFS
    CHECK(presence_calib_baseline(lv, 50, &base, &spread) == PRESENCE_CAL_OK && base > -67 && base < -65 && spread < 3,
          "quiet room: baseline %.1f spread %.1f", base, spread);
    for (int i = 0; i < 50; i++) lv[i] = i % 10 < 3 ? -35 : -67;               // speech 30 % of the time
    base = -60;
    CHECK(presence_calib_baseline(lv, 50, &base, &spread) == PRESENCE_CAL_NOISY && base == -60 && spread > 20,
          "speech during calibration: refused, baseline kept (%.1f), spread %.1f", base, spread);
    for (int i = 0; i < 50; i++) lv[i] = i % 25 == 0 ? -35 : -66;              // two door slams in 5 s: still the room
    CHECK(presence_calib_baseline(lv, 50, &base, &spread) == PRESENCE_CAL_OK && base == -66,
          "two bangs don't move the median: %.1f", base);
    base = -60;
    CHECK(presence_calib_baseline(lv, 5, &base, &spread) == PRESENCE_CAL_NOISY && base == -60, "too few samples");
    float even[10] = { -70, -70, -70, -70, -68, -66, -64, -64, -64, -64 };
    CHECK(presence_calib_baseline(even, 10, &base, &spread) == PRESENCE_CAL_OK && base == -67, "even count: %.1f", base);

    // JSON: only the fields given change; motion only when one of its two is given
    presence_cfg_t jc = normal;
    presence_motion_t m = { .on = true, .thr = 0.1f };
    cJSON *j = cJSON_Parse("{\"enabled\":false,\"dim_s\":30,\"bright_pct\":70,\"margin_db\":\"x\"}");
    presence_json_apply(j, &jc, &m);
    cJSON_Delete(j);
    CHECK(!jc.enabled && jc.dim_s == 30 && jc.bright_pct == 70 && jc.margin_db == 10 && jc.off_s == 3000 && !m.changed,
          "partial update: %d %f %d %f", jc.enabled, jc.dim_s, jc.bright_pct, jc.margin_db);
    j = cJSON_Parse("{\"motion_thr\":0.3}");
    presence_json_apply(j, &jc, &m);
    cJSON_Delete(j);
    CHECK(m.changed && m.on && fabsf(m.thr - 0.3f) < 1e-6, "motion threshold alone: %d %d %f", m.changed, m.on, m.thr);
    presence_status_t st = { .state = PRESENCE_DIM, .last_cal = PRESENCE_CAL_NOISY, .cal_spread_db = 31 };
    j = presence_json(&jc, &st, false, true);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "state")), "dim") &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "cal")), "noisy") &&
          cJSON_IsFalse(cJSON_GetObjectItem(j, "motion_wake")) && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok")) &&
          cJSON_GetObjectItem(j, "dim_s")->valuedouble == 30, "GET's answer");
    cJSON_Delete(j);

    // weather_amoled's old "cfg" blob (32 bytes, little-endian): decoded field by field, clamped; other sizes refused
    uint8_t blob[32] = {0};
    float bf[4] = { 12.5f, 2.5f, 777, 200000 };          // margin, wake, dim, off (off over the 24 h limit)
    int32_t bp[2] = { 63, 9 };
    float bb = -61.25f;
    blob[0] = 1;
    memcpy(blob + 4, bf, sizeof(bf));
    memcpy(blob + 20, bp, sizeof(bp));
    memcpy(blob + 28, &bb, sizeof(bb));
    presence_cfg_t bc;
    CHECK(presence_cfg_from_blob_v1(blob, 32, &bc), "a 32-byte blob decodes");
    CHECK(bc.enabled && bc.margin_db == 12.5f && bc.wake_s == 2.5f && bc.dim_s == 777 && bc.off_s == 86400 &&
          bc.bright_pct == 63 && bc.dim_pct == 9 && bc.baseline_db == -61.25f, "%d %f %f %f %f %d %d %f", bc.enabled,
          bc.margin_db, bc.wake_s, bc.dim_s, bc.off_s, bc.bright_pct, bc.dim_pct, bc.baseline_db);
    float nan = NAN;
    memcpy(blob + 4, &nan, sizeof(nan));
    blob[0] = 0;
    CHECK(presence_cfg_from_blob_v1(blob, 32, &bc) && !bc.enabled && bc.margin_db == 1, "NaN clamped: %f", bc.margin_db);
    CHECK(!presence_cfg_from_blob_v1(blob, 28, &bc) && !presence_cfg_from_blob_v1(blob, 36, &bc) &&
          !presence_cfg_from_blob_v1(NULL, 32, &bc), "other sizes refused");

    return check_done("presence");
}
