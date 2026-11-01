// /api/presence's JSON, both ways (docs/PROTOCOL.md §4). cJSON only: host test tests/host/test_presence.c.
#include "presence_json.h"

static float num_or(const cJSON *j, const char *key, float def)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

void presence_json_apply(const cJSON *j, presence_cfg_t *c, presence_motion_t *m)
{
    const cJSON *en = cJSON_GetObjectItem(j, "enabled");
    if (cJSON_IsBool(en)) c->enabled = cJSON_IsTrue(en);
    c->margin_db = num_or(j, "margin_db", c->margin_db);
    c->wake_s = num_or(j, "wake_s", c->wake_s);
    c->dim_s = num_or(j, "dim_s", c->dim_s);
    c->off_s = num_or(j, "off_s", c->off_s);
    c->bright_pct = (int)num_or(j, "bright_pct", c->bright_pct);
    c->dim_pct = (int)num_or(j, "dim_pct", c->dim_pct);
    const cJSON *mw = cJSON_GetObjectItem(j, "motion_wake");
    const cJSON *mt = cJSON_GetObjectItem(j, "motion_thr");
    m->changed = cJSON_IsBool(mw) || cJSON_IsNumber(mt);
    if (cJSON_IsBool(mw)) m->on = cJSON_IsTrue(mw);
    if (cJSON_IsNumber(mt)) m->thr = (float)mt->valuedouble;
}

// weather_amoled's shape (plus "ok", "cal" and "cal_spread_db"): the settings, then the live state
cJSON *presence_json(const presence_cfg_t *c, const presence_status_t *st, bool motion_wake, bool ok)
{
    static const char *names[] = {"active", "dim", "off"};
    static const char *cals[] = {"none", "ok", "noisy"};
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", ok);
    cJSON_AddBoolToObject(j, "enabled", c->enabled);
    cJSON_AddNumberToObject(j, "margin_db", c->margin_db);
    cJSON_AddNumberToObject(j, "wake_s", c->wake_s);
    cJSON_AddNumberToObject(j, "dim_s", c->dim_s);
    cJSON_AddNumberToObject(j, "off_s", c->off_s);
    cJSON_AddNumberToObject(j, "bright_pct", c->bright_pct);
    cJSON_AddNumberToObject(j, "dim_pct", c->dim_pct);
    cJSON_AddNumberToObject(j, "baseline_db", c->baseline_db);
    cJSON_AddNumberToObject(j, "level_db", st->level_db);
    cJSON_AddNumberToObject(j, "threshold_db", st->threshold_db);
    cJSON_AddStringToObject(j, "state", names[st->state]);
    cJSON_AddNumberToObject(j, "wake_progress", st->wake_progress);
    cJSON_AddNumberToObject(j, "quiet_s", st->quiet_s);
    cJSON_AddBoolToObject(j, "calibrating", st->calibrating);
    cJSON_AddNumberToObject(j, "calib_left_s", st->calib_left_s);
    cJSON_AddStringToObject(j, "cal", cals[st->last_cal]);
    cJSON_AddNumberToObject(j, "cal_spread_db", st->cal_spread_db);
    cJSON_AddBoolToObject(j, "mic_ok", st->mic_ok);
    cJSON_AddNumberToObject(j, "brightness", st->brightness);
    cJSON_AddBoolToObject(j, "imu_ok", st->imu_ok);
    cJSON_AddNumberToObject(j, "motion_g", st->motion_g);
    cJSON_AddBoolToObject(j, "motion_wake", motion_wake);
    cJSON_AddNumberToObject(j, "motion_thr", st->motion_thr);
    return j;
}
