// presence.c's state machine, setting limits and calibration statistic, pure C (no ESP-IDF): host test
// tests/host/test_presence.c. The logic is weather_amoled's presence_task, with the touch added as activity
// (esp32-s3-rtcquebec v0.3.0) and a calibration that a voice can't spoil.
#include "presence.h"
#include <stdlib.h>
#include <string.h>

// The limits every setting is held to, whether it comes from the page or NVS (weather_amoled until v1.12.0: a value
// loaded from NVS skipped them). The dimmed level may be above the full one: it is capped where it is used, so a
// brightness lowered for a while doesn't lower the saved dim level for good.
void presence_clamp_cfg(presence_cfg_t *c)
{
    if (!(c->margin_db >= 1)) c->margin_db = 1;          // (NaN too)
    if (c->margin_db > 60) c->margin_db = 60;
    if (!(c->wake_s >= 0.2f)) c->wake_s = 0.2f;
    if (c->wake_s > 60) c->wake_s = 60;
    if (!(c->dim_s >= 1)) c->dim_s = 1;
    if (c->dim_s > 86400) c->dim_s = 86400;              // a day (NVS keeps whole seconds in a u32)
    if (!(c->off_s >= 1)) c->off_s = 1;
    if (c->off_s > 86400) c->off_s = 86400;
    if (c->bright_pct < 5) c->bright_pct = 5;
    if (c->bright_pct > 100) c->bright_pct = 100;
    if (c->dim_pct < 1) c->dim_pct = 1;
    if (c->dim_pct > 100) c->dim_pct = 100;
    if (!(c->baseline_db >= -100)) c->baseline_db = -100;
    if (c->baseline_db > 0) c->baseline_db = 0;
}

presence_state_t presence_sm_step(presence_sm_t *sm, const presence_cfg_t *c, bool running, bool loud, bool moved,
                                  bool touched, float dt)
{
    // Sustained-noise score: rises while loud, falls at half speed while quiet
    float s = sm->score + (loud ? dt : -dt * 0.5f);
    sm->score = s < 0 ? 0 : (s > c->wake_s ? c->wake_s : s);
    bool woken = moved || touched;
    if (sm->dark_touch_s > 0) sm->dark_touch_s = sm->dark_touch_s > dt ? sm->dark_touch_s - dt : 0;

    if (!running) {
        sm->state = PRESENCE_ACTIVE;
        sm->quiet_s = 0;
        return sm->state;
    }
    switch (sm->state) {
    case PRESENCE_ACTIVE:
        sm->quiet_s = loud || woken ? 0 : sm->quiet_s + dt;
        if (sm->quiet_s >= c->dim_s) sm->state = PRESENCE_DIM;
        break;
    case PRESENCE_DIM:
        if (sm->score >= c->wake_s || woken) { sm->state = PRESENCE_ACTIVE; sm->quiet_s = 0; break; }
        sm->quiet_s = loud ? c->dim_s : sm->quiet_s + dt;    // a short noise restarts the off countdown
        if (sm->quiet_s >= c->dim_s + c->off_s) sm->state = PRESENCE_OFF;
        break;
    case PRESENCE_OFF:
        if (sm->score >= c->wake_s || woken) { sm->state = PRESENCE_ACTIVE; sm->quiet_s = 0; }
        break;
    }
    return sm->state;
}

bool presence_sm_take_dark_touch(presence_sm_t *sm)
{
    bool dark = sm->state == PRESENCE_OFF || sm->dark_touch_s > 0;
    sm->dark_touch_s = 0;
    return dark;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

// weather_amoled took the 90th percentile: speech during the 5 s set the baseline at -35 dBFS in a -67 dBFS room
// (esp32-s3-rtcquebec, October 7), and with the margin on top the screen would never have dimmed again
presence_cal_t presence_calib_baseline(float *levels, int n, float *baseline, float *spread)
{
    *spread = 0;
    if (n < 10) return PRESENCE_CAL_NOISY;
    qsort(levels, n, sizeof(float), cmp_float);
    *spread = levels[n * 9 / 10] - levels[n / 10];
    if (!(*spread <= PRESENCE_CAL_SPREAD_DB)) return PRESENCE_CAL_NOISY;
    *baseline = n % 2 ? levels[n / 2] : (levels[n / 2 - 1] + levels[n / 2]) / 2;
    return PRESENCE_CAL_OK;
}

bool presence_cfg_from_blob_v1(const void *blob, size_t n, presence_cfg_t *out)
{
    if (!blob || n != 32) return false;
    const uint8_t *b = blob;
    float f[4], base;
    int32_t pct[2];
    memcpy(f, b + 4, sizeof(f));                         // margin_db, wake_s, dim_s, off_s at 4, 8, 12, 16
    memcpy(pct, b + 20, sizeof(pct));                    // bright_pct, dim_pct at 20, 24
    memcpy(&base, b + 28, sizeof(base));
    *out = (presence_cfg_t){ .enabled = b[0] != 0, .margin_db = f[0], .wake_s = f[1], .dim_s = f[2], .off_s = f[3],
                             .bright_pct = pct[0], .dim_pct = pct[1], .baseline_db = base };
    presence_clamp_cfg(out);
    return true;
}
