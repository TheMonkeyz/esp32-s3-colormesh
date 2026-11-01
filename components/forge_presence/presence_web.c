// The settings page's routes for screen dimming (docs/PROTOCOL.md §4), registered by presence_web_routes()
#include "presence.h"
#include "presence_json.h"
#include "web.h"

static esp_err_t send(httpd_req_t *req, bool ok)
{
    presence_cfg_t c;
    presence_status_t st;
    presence_get_config(&c);
    presence_get_status(&st);
    return web_send_json(req, presence_json(&c, &st, presence_motion_wake(), ok));
}

// GET /api/presence: the settings, then the live state (the page polls it while open)
static esp_err_t presence_get(httpd_req_t *req) { return send(req, true); }

// POST /api/presence with any of the settings: saved, then GET's answer ("ok":false: not saved)
static esp_err_t presence_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    presence_cfg_t c;
    presence_status_t st;
    presence_get_config(&c);
    presence_get_status(&st);
    presence_motion_t m = { .on = presence_motion_wake(), .thr = st.motion_thr };
    presence_json_apply(j, &c, &m);
    cJSON_Delete(j);
    bool saved = m.changed ? presence_set_motion(m.on, m.thr) : true;
    saved = presence_set_config(&c) && saved;
    presence_settings_changed();
    return send(req, saved);
}

// POST /api/calibrate {"seconds":5}: measure the room's background noise (keep quiet meanwhile). GET's answer with
// "calibrating":true, or {"ok":false,"why":"no_mic"|"busy"} (200: a refusal is an answer). The page polls GET for
// the end: "cal" "ok", or "noisy" (the baseline was kept)
static esp_err_t calibrate_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    const cJSON *s = cJSON_GetObjectItem(j, "seconds");
    int secs = cJSON_IsNumber(s) ? (int)s->valuedouble : 5;
    cJSON_Delete(j);
    presence_status_t st;
    presence_get_status(&st);
    if (!st.mic_ok) return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"no_mic\"}");
    if (!presence_calibrate(secs)) return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"busy\"}");
    return send(req, true);
}

static const web_route_t routes[] = {
    { "/api/presence",  HTTP_GET,  presence_get, .keyed = false },
    { "/api/presence",  HTTP_POST, presence_post, .keyed = true },
    { "/api/calibrate", HTTP_POST, calibrate_post, .keyed = true },
};

void presence_web_routes(void) { web_add_routes(routes, sizeof(routes) / sizeof(routes[0])); }
