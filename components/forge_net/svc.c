// External service health (see svc.h)
#include "svc.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "http_once.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "net.h"

static const char *TAG = "svc";
#define PROBE_AFTER_US (5 * 60 * 1000000LL)

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// (EXT_RAM_BSS_ATTR: static buffers are internal RAM otherwise, the scarce kind; weather_amoled lost 2.2 KB of it
// when it took these components, October 5, LESSONS L185)
EXT_RAM_BSS_ATTR static svc_info_t svc[SVC_MAX];
static svc_probe_url_t probe_fn[SVC_MAX];
static int nsvc;
static volatile bool probe_running;
static const char *(*why_text)(svc_why_t code, int http);

void svc_set_why_text(const char *(*fn)(svc_why_t code, int http)) { why_text = fn; }

int svc_find(const char *name)
{
    for (int i = 0; i < nsvc; i++) if (name && !strcmp(svc[i].name, name)) return i;
    return -1;
}

int svc_add(const char *name, const char *api, svc_probe_url_t probe)
{
    int id = -1;
    taskENTER_CRITICAL(&mux);
    if (nsvc < SVC_MAX) {
        id = nsvc++;
        svc[id] = (svc_info_t){ .name = name, .api = api };
        probe_fn[id] = probe;
    }
    taskEXIT_CRITICAL(&mux);
    if (id < 0) ESP_LOGE(TAG, "no room for service %s (SVC_MAX)", name);
    return id;
}

int svc_count(void) { return nsvc; }

static void record(int id, bool ok, svc_why_t code, int http, const char *why, int64_t t0)
{
    if (id < 0 || id >= nsvc) return;
    int64_t now = esp_timer_get_time();
    bool changed;
    int fails;
    taskENTER_CRITICAL(&mux);
    svc_info_t *s = &svc[id];
    changed = s->last_try && s->ok != ok;
    s->last_try = now;
    s->ms = t0 ? (int)((now - t0) / 1000) : 0;
    s->ok = ok;
    s->probing = false;
    if (ok) { s->last_ok = now; s->fails = 0; s->code = SVC_WHY_NONE; s->http = 0; }
    else { s->fails++; s->code = code; s->http = http; strlcpy(s->why, why, sizeof(s->why)); }
    fails = s->fails;
    taskEXIT_CRITICAL(&mux);
    if (changed || fails == 1) {               // log transitions only, not every retry
        if (ok) ESP_LOGI(TAG, "%s: OK again", svc[id].name);
        else ESP_LOGW(TAG, "%s: %s", svc[id].name, why);
    }
}

static const char *const WHY_EN[] = { "", "HTTP", "can't connect", "timeout", "no reply", "bad reply", "empty", "error" };

// A code's text: the app's (its language) if it has one, else English; "HTTP 404" for a status whatever the language
static void fail_code(int id, svc_why_t code, int http, const char *other, int64_t t0)
{
    char why[40];
    const char *t = why_text && code != SVC_WHY_HTTP && code != SVC_WHY_OTHER ? why_text(code, http) : NULL;
    if (code == SVC_WHY_HTTP) snprintf(why, sizeof(why), "HTTP %d", http);
    else if (code == SVC_WHY_OTHER) snprintf(why, sizeof(why), "%s", other ? other : WHY_EN[code]);
    else snprintf(why, sizeof(why), "%s", t ? t : WHY_EN[code]);
    record(id, false, code, http, why, t0);
}

void svc_http(int id, esp_err_t err, int status, int64_t t0)
{
    if (err == ESP_OK && status == 200) { record(id, true, SVC_WHY_NONE, 0, "", t0); return; }
    if (err == ESP_OK) fail_code(id, SVC_WHY_HTTP, status, NULL, t0);
    else if (err == ESP_ERR_HTTP_CONNECT) fail_code(id, SVC_WHY_CONNECT, 0, NULL, t0);
    else if (err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_TIMEOUT) fail_code(id, SVC_WHY_TIMEOUT, 0, NULL, t0);
    else if (err == ESP_ERR_HTTP_FETCH_HEADER) fail_code(id, SVC_WHY_NO_REPLY, 0, NULL, t0);
    else fail_code(id, SVC_WHY_OTHER, 0, esp_err_to_name(err), t0);
}

void svc_ok(int id, int64_t t0) { record(id, true, SVC_WHY_NONE, 0, "", t0); }
void svc_fail(int id, const char *why, int64_t t0) { fail_code(id, SVC_WHY_OTHER, 0, why, t0); }
void svc_fail_why(int id, svc_why_t code, int64_t t0) { fail_code(id, code, 0, NULL, t0); }

void svc_get(int id, svc_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (id < 0 || id >= nsvc) return;
    taskENTER_CRITICAL(&mux);
    *out = svc[id];
    taskEXIT_CRITICAL(&mux);
}

const char *svc_user_agent(void)
{
    EXT_RAM_BSS_ATTR static char ua[256];      // filled once; two tasks racing write the same bytes
    // "<product>/<version> (<comment>; +https://github.com/<repo>)": the fixed text is 26 bytes, the version (git
    // describe) under 32, the project name (used when FORGE_PRODUCT is empty) at most 32. -Werror=format-truncation
    // failed MeteoBus's build at 128 bytes once its three Kconfig strings grew; this says why at compile time.
    _Static_assert(sizeof(CONFIG_FORGE_PRODUCT) + sizeof(CONFIG_FORGE_UA_COMMENT) + sizeof(CONFIG_FORGE_REPO) + 96
                   <= sizeof(ua), "FORGE_PRODUCT/REPO/UA_COMMENT too long for the User-Agent (svc.c)");
    if (!ua[0]) {
        const char *v = esp_app_get_description()->version;
        snprintf(ua, sizeof(ua), "%s/%s (%s%s+https://github.com/%s)",
                 CONFIG_FORGE_PRODUCT[0] ? CONFIG_FORGE_PRODUCT : esp_app_get_description()->project_name,
                 v[0] == 'v' ? v + 1 : v, CONFIG_FORGE_UA_COMMENT, CONFIG_FORGE_UA_COMMENT[0] ? "; " : "",
                 CONFIG_FORGE_REPO);
        ESP_LOGI(TAG, "User-Agent: %s", ua);
    }
    return ua;
}

// One job on forge_net's background worker (inside the download slot): every stale service, in turn
static void probe_job(void *arg)
{
    char url[200];
    for (int i = 0; i < nsvc; i++) {
        bool due;
        taskENTER_CRITICAL(&mux);
        due = svc[i].probing;
        taskEXIT_CRITICAL(&mux);
        if (!due) continue;
        probe_fn[i](url, sizeof(url));
        esp_http_client_config_t cfg = {
            .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000,
            .user_agent = svc_user_agent(),
        };
        int64_t t0 = esp_timer_get_time();
        int status;
        esp_err_t err = http_once(&cfg, &status);         // body read and dropped
        svc_http(i, err, status, t0);
        ESP_LOGI(TAG, "probe %s: %s, HTTP %d, %d ms", svc[i].name, esp_err_to_name(err), status,
                 (int)((esp_timer_get_time() - t0) / 1000));
    }
    probe_running = false;
}

void svc_probe_stale(void)
{
    if (probe_running || !net_is_connected()) return;
    int64_t now = esp_timer_get_time();
    int n = 0;
    taskENTER_CRITICAL(&mux);
    for (int i = 0; i < nsvc; i++) {
        svc[i].probing = probe_fn[i] && (!svc[i].last_try || now - svc[i].last_try > PROBE_AFTER_US);
        n += svc[i].probing;
    }
    taskEXIT_CRITICAL(&mux);
    if (!n) return;
    probe_running = true;
    if (!net_bg_post(probe_job, NULL, "probe")) {
        ESP_LOGW(TAG, "probe not queued");
        taskENTER_CRITICAL(&mux);
        for (int i = 0; i < nsvc; i++) svc[i].probing = false;
        taskEXIT_CRITICAL(&mux);
        probe_running = false;
    }
}
