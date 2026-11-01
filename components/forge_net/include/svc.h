#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Health of the external services the firmware depends on (a status screen, /api/info, the log). Every fetch reports
// its outcome here; the log shows changes only ("X: OK again", "X: timeout"), not every retry. Times are esp_timer
// microseconds (0 = never). forge_net adds SVC_NAME_NTP, forge_ota SVC_NAME_UPDATES (svc_find() gives their ids); the
// app adds its own at start-up, in the order it wants them listed (before net_init() to come first).
#define SVC_NAME_NTP "pool.ntp.org"
#define SVC_NAME_UPDATES "GitHub Pages"

// Why the last try failed, as a code: the app shows its own text (svc_set_why_text, or from code/http itself)
typedef enum {
    SVC_WHY_NONE,           // no failure
    SVC_WHY_HTTP,           // an HTTP status other than 200 (svc_info_t.http)
    SVC_WHY_CONNECT,        // can't connect
    SVC_WHY_TIMEOUT,
    SVC_WHY_NO_REPLY,       // connected, no response headers
    SVC_WHY_BAD_REPLY,      // a reply that couldn't be read (svc_fail_why)
    SVC_WHY_EMPTY,          // a valid reply with nothing in it (svc_fail_why)
    SVC_WHY_OTHER,          // an ESP-IDF error (its name in why)
} svc_why_t;

typedef struct {
    const char *name;       // "Open-Meteo"
    const char *api;        // "Forecast API v1"
    int64_t last_try, last_ok;
    bool ok;                // outcome of the last try
    bool probing;           // a check is queued or running (svc_probe_stale)
    int fails;              // failures in a row
    int ms;                 // duration of the last try
    svc_why_t code;         // reason of the last failure as a code
    int http;               // its HTTP status (SVC_WHY_HTTP)
    char why[40];           // ...as text: svc_set_why_text()'s, else English (diagnostics, also logged)
} svc_info_t;

// probe: writes a small URL to check this service with (NULL: never probed, e.g. SNTP). Returns the id, -1 if full.
typedef void (*svc_probe_url_t)(char *url, size_t n);
int svc_add(const char *name, const char *api, svc_probe_url_t probe);
int svc_count(void);

// t0 = esp_timer_get_time() taken before the request. Success = ESP_OK and HTTP 200.
void svc_http(int id, esp_err_t err, int status, int64_t t0);
void svc_ok(int id, int64_t t0);
void svc_fail(int id, const char *why, int64_t t0);          // a reason of the app's own (code SVC_WHY_OTHER)
void svc_fail_why(int id, svc_why_t code, int64_t t0);       // a reason as a code (text from the hook)
int svc_find(const char *name);                              // the id of the service with that name, -1 if none
// The text of a reason, in the display language (weather_amoled: tr(T_ERR_*)); NULL or a NULL result: English.
// Called when the failure is recorded (why[] and the log line carry it), from the task that reports it.
void svc_set_why_text(const char *(*fn)(svc_why_t code, int http));
void svc_get(int id, svc_info_t *out);
// The User-Agent of the firmware's own requests (public APIs ask apps to identify themselves):
// "<CONFIG_FORGE_PRODUCT or project>/<version> ([<CONFIG_FORGE_UA_COMMENT>; ]+https://github.com/<CONFIG_FORGE_REPO>)"
const char *svc_user_agent(void);
// A status page opened: on forge_net's background worker, send a small request to each service not contacted for 5 min
void svc_probe_stale(void);
#define SVC_MAX 12
