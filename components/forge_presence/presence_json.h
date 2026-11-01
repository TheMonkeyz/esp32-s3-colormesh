#pragma once
// Private: /api/presence's JSON (presence_json.c), shared by presence_web.c and the host test
#include "cJSON.h"
#include "presence.h"

typedef struct { bool changed, on; float thr; } presence_motion_t;
// Any of "enabled", "margin_db", "wake_s", "dim_s", "off_s", "bright_pct", "dim_pct" into *c (others kept);
// "motion_wake" / "motion_thr" into *m (m->changed: at least one was given). Limits are presence_clamp_cfg's.
void presence_json_apply(const cJSON *j, presence_cfg_t *c, presence_motion_t *m);
// presence.c: the app's settings_changed hook, after a change through the page (presence_web.c)
void presence_settings_changed(void);
cJSON *presence_json(const presence_cfg_t *c, const presence_status_t *st, bool motion_wake, bool ok);
