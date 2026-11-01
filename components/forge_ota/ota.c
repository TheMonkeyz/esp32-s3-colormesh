// Over-the-air updates (see ota.h).
// Safety: the bootloader has rollback enabled. A freshly installed image starts "pending verify"; it is confirmed
// after 60 s *and* a Wi-Fi connection (else after 10 min: a home without Wi-Fi keeps a working display). Until
// v1.12.0 any image that stayed up 60 s was confirmed, before Wi-Fi had even started: one whose network never worked
// would have been kept. A restart before that (crash, boot loop, power cut) goes back to the previous image, and that
// one tells the user once (rolled_back). The download is checked by esp_https_ota (image header, SHA-256: integrity,
// not authenticity: the site is trusted through HTTPS); we also require the same project name and the version that
// was offered before switching.
#include "ota.h"
#include "esp_attr.h"
#include "version.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "http_once.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "net.h"
#include "svc.h"
#include "forge_i18n.h"
#include "web.h"
#include "esp_timer.h"

static const char *TAG = "ota";
#define CHECK_EVERY_S (6 * 3600)
#define VALID_AFTER_S 60
#define VALID_OFFLINE_S 600             // confirmed without Wi-Fi after this
#define RETRY_FAILED_S 120              // a failed download: offered again, re-checked after this

static ota_status_t st;
static int svc_updates = -1;
void ota_web_routes(void);   // ota_web.c
static ota_listener_t listener;
static SemaphoreHandle_t mux;
static TaskHandle_t task;
EXT_RAM_BSS_ATTR static char app_url[256];             // (PSRAM: L185)
#define NOTES_MAX 3072
static char *notes;                 // PSRAM, NOTES_MAX, under mux
static volatile bool want_check, want_install, retry_check;

static void publish(void)
{
    ota_status_t copy;
    xSemaphoreTake(mux, portMAX_DELAY);
    copy = st;
    xSemaphoreGive(mux);
    if (listener) listener(&copy);
}

// English reasons for the log and the API; the app shows its own text for st.err (ota_err_t)
static const char *const ERR_TEXT[] = {
    [OTA_E_NONE] = "", [OTA_E_NO_SITE] = "update site unreachable", [OTA_E_BAD_SITE] = "update site: bad channels.json",
    [OTA_E_NO_IMAGE] = "no app image in the manifest", [OTA_E_NO_START] = "download did not start",
    [OTA_E_WRONG] = "not firmware for this device", [OTA_E_INTERRUPTED] = "download interrupted",
    [OTA_E_INVALID] = "downloaded image invalid",
};

static const char *(*err_text)(ota_err_t err);
void ota_set_err_text(const char *(*fn)(ota_err_t err)) { err_text = fn; }

static void set_state(ota_state_t s, ota_err_t err)
{
    const char *t = err != OTA_E_KEEP && err != OTA_E_NONE && err_text ? err_text(err) : NULL;
    xSemaphoreTake(mux, portMAX_DELAY);
    st.state = s;
    if (err != OTA_E_KEEP) { st.err = err; strlcpy(st.error, t ? t : ERR_TEXT[err], sizeof(st.error)); }
    xSemaphoreGive(mux);
    publish();
}

/* ---------- versions: version.c ---------- */

/* ---------- HTTP GET into a buffer ---------- */

typedef struct { char *buf; int len, cap; } rx_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    rx_t *rx = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && rx->len + e->data_len < rx->cap) {
        memcpy(rx->buf + rx->len, e->data, e->data_len);
        rx->len += e->data_len;
        rx->buf[rx->len] = 0;
    }
    return ESP_OK;
}

static cJSON *get_json(const char *url, int cap)
{
    rx_t rx = { .cap = cap };
    rx.buf = heap_caps_calloc(1, rx.cap, MALLOC_CAP_SPIRAM);
    if (!rx.buf) return NULL;
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = http_evt, .user_data = &rx,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
    };
    if (!net_dl_begin("update check", 30000)) { free(rx.buf); return NULL; }   // one TLS download at a time
    int64_t t0 = esp_timer_get_time();
    int status;
    esp_err_t err = http_once(&cfg, &status);
    net_dl_end("update check");
    cJSON *j = err == ESP_OK && status == 200 ? cJSON_Parse(rx.buf) : NULL;
    if (err == ESP_OK && status == 200 && !j) svc_fail_why(svc_updates, SVC_WHY_BAD_REPLY, t0);
    else svc_http(svc_updates, err, status, t0);
    if (!j) ESP_LOGW(TAG, "GET %s: %s, status %d", url, esp_err_to_name(err), status);
    free(rx.buf);
    return j;
}

/* ---------- release notes ---------- */


// Keeps the releases newer than the running version, up to the offered one. When a release is offered,
// release-candidate sections are skipped: the release's own section lists everything.
static void fetch_notes(const ver_t *cur, bool cur_ok, const ver_t *lat)
{
    char *buf = heap_caps_calloc(1, NOTES_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    int n = 0, kept = 0;
    cJSON *j = get_json(OTA_SITE "notes.json", 24576);
    cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(j, "releases")) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(r, "version"));
        const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(r, "date"));
        ver_t rv;
        if (!v || !parse_ver(v, &rv)) continue;
        if (cmp_ver(&rv, lat) > 0 || (cur_ok && cmp_ver(&rv, cur) <= 0)) continue;
        if (lat->kind == 1 && rv.kind == 0) continue;
        char date[32] = "";
        int y, mo, dd;
        if (d && sscanf(d, "%d-%d-%d", &y, &mo, &dd) == 3 && mo >= 1 && mo <= 12)
            tr_date_ymd(y, mo, dd, date, sizeof(date));                // "September 30, 2026" / "30 septembre 2026"
        int need = snprintf(NULL, 0, "%s%s|%s\n", kept ? "\n" : "", v, date);
        if (n + need >= NOTES_MAX - 32) break;
        n += snprintf(buf + n, NOTES_MAX - n, "%s%s|%s\n", kept ? "\n" : "", v, date);
        kept++;
        cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(r, "notes")) {
            const char *t = cJSON_GetStringValue(it);
            if (!t) continue;
            if (n + (int)strlen(t) + 2 >= NOTES_MAX - 32) { n += snprintf(buf + n, NOTES_MAX - n, "...\n"); goto full; }
            n += snprintf(buf + n, NOTES_MAX - n, "%s\n", t);
        }
    }
full:
    cJSON_Delete(j);
    if (!j) ESP_LOGW(TAG, "No release notes");
    else ESP_LOGI(TAG, "Release notes: %d release(s), %d bytes", kept, n);
    xSemaphoreTake(mux, portMAX_DELAY);
    if (!notes || strcmp(notes, buf)) {
        free(notes);
        notes = buf;
        buf = NULL;
        st.notes_id++;
    }
    xSemaphoreGive(mux);
    free(buf);
}

void ota_get_notes(char *out, size_t size)
{
    if (!size) return;
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(out, notes ? notes : "", size);
    xSemaphoreGive(mux);
}

/* ---------- check ---------- */

static void check(void)
{
    set_state(OTA_CHECKING, OTA_E_NONE);
    cJSON *ch = get_json(OTA_SITE "channels.json", 8192);
    if (!ch) { set_state(OTA_FAILED, OTA_E_NO_SITE); return; }
    char chan[8];
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(chan, st.channel, sizeof(chan));
    xSemaphoreGive(mux);
    cJSON *c = cJSON_GetObjectItem(ch, chan);
    if (!cJSON_IsObject(c)) c = cJSON_GetObjectItem(ch, "stable");      // no beta right now: stable
    if (!cJSON_IsObject(c) && cJSON_IsObject(ch)) {                      // nothing released on it yet (a new
        cJSON_Delete(ch);                                                // project before its first stable)
        ESP_LOGI(TAG, "%s channel offers nothing yet", chan);
        set_state(OTA_UP_TO_DATE, OTA_E_NONE);
        return;
    }
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(c, "version"));
    const char *man = cJSON_GetStringValue(cJSON_GetObjectItem(c, "manifest"));
    if (!ver || !man) { cJSON_Delete(ch); set_state(OTA_FAILED, OTA_E_BAD_SITE); return; }
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(st.latest, ver, sizeof(st.latest));
    xSemaphoreGive(mux);

    ver_t cur, lat;
    bool cur_ok = parse_ver(st.current, &cur), lat_ok = parse_ver(ver, &lat);
    bool newer = lat_ok && (!cur_ok || cmp_ver(&lat, &cur) > 0);        // unparsable local build: offer it
    ESP_LOGI(TAG, "%s channel offers %s, running %s: %s", chan, ver, st.current, newer ? "update available" : "up to date");
    if (!newer) { cJSON_Delete(ch); set_state(OTA_UP_TO_DATE, OTA_E_NONE); return; }

    // The app image is the manifest part at 0x10000, relative to the manifest
    char url[256];
    snprintf(url, sizeof(url), OTA_SITE "%s", man);
    cJSON_Delete(ch);
    cJSON *m = get_json(url, 8192);
    if (!m) { set_state(OTA_FAILED, OTA_E_NO_SITE); return; }      // (was reported as "no app image")
    cJSON *parts = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(m, "builds"), 0), "parts");
    const char *path = NULL;
    cJSON *p;
    cJSON_ArrayForEach(p, parts) {
        cJSON *off = cJSON_GetObjectItem(p, "offset");
        if (cJSON_IsNumber(off) && off->valueint == 0x10000) path = cJSON_GetStringValue(cJSON_GetObjectItem(p, "path"));
    }
    if (!path) { cJSON_Delete(m); set_state(OTA_FAILED, OTA_E_NO_IMAGE); return; }
    char *slash = strrchr(url, '/');
    if (slash) slash[1] = 0;
    xSemaphoreTake(mux, portMAX_DELAY);
    snprintf(app_url, sizeof(app_url), "%s%s", url, path);
    xSemaphoreGive(mux);
    cJSON_Delete(m);
    fetch_notes(&cur, cur_ok, &lat);                                   // optional: an update installs without them
    set_state(OTA_AVAILABLE, OTA_E_NONE);
}

/* ---------- install ---------- */

static void install_in_slot(void);

static void install(void)
{
    // One TLS download at a time (net.h): the whole image, minutes; a map or a probe waits or skips a round meanwhile
    if (!net_dl_begin("update", 60000)) { set_state(OTA_FAILED, OTA_E_NO_START); return; }
    install_in_slot();
    net_dl_end("update");                                 // (a success restarted the board before this line)
}

static void install_in_slot(void)
{
    char url[256];
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(url, app_url, sizeof(url));
    st.progress = 0;
    xSemaphoreGive(mux);
    ESP_LOGI(TAG, "Installing %s", url);
    set_state(OTA_DOWNLOADING, OTA_E_NONE);
    esp_http_client_config_t http = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000,
        .keep_alive_enable = true, .buffer_size = 4096,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) { set_state(OTA_FAILED, OTA_E_NO_START); return; }

    esp_app_desc_t desc;
    char offered[32];
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(offered, st.latest, sizeof(offered));
    xSemaphoreGive(mux);
    if (esp_https_ota_get_img_desc(h, &desc) != ESP_OK ||
        strcmp(desc.project_name, esp_app_get_description()->project_name) || strcmp(desc.version, offered)) {
        ESP_LOGE(TAG, "Not the firmware offered (%s %s, expected %s)", desc.project_name, desc.version, offered);
        esp_https_ota_abort(h);
        set_state(OTA_FAILED, OTA_E_WRONG);
        return;
    }
    int total = esp_https_ota_get_image_size(h), last = -1;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int pct = total > 0 ? (int)(100LL * esp_https_ota_get_image_len_read(h) / total) : 0;
        if (pct != last) {
            last = pct;
            xSemaphoreTake(mux, portMAX_DELAY);
            st.progress = pct;
            xSemaphoreGive(mux);
            publish();
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        // Still offered: Install stays (with the reason), and the channel is checked again soon. A failed state hid
        // the pill and the button until the next check, up to 6 h later.
        set_state(OTA_AVAILABLE, OTA_E_INTERRUPTED);
        retry_check = true;
        return;
    }
    err = esp_https_ota_finish(h);                  // verifies the image and selects it for the next boot
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image rejected: %s", esp_err_to_name(err));
        set_state(OTA_FAILED, OTA_E_INVALID);
        return;
    }
    ESP_LOGI(TAG, "Update installed, restarting (ota task: %u B of stack spare)", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreTake(mux, portMAX_DELAY);
    st.progress = 100;
    xSemaphoreGive(mux);
    set_state(OTA_DONE, OTA_E_NONE);
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
}

/* ---------- task ---------- */

bool ota_pending_verify(void)
{
    esp_ota_img_states_t s;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));                     // let the answer / the screen go out
    for (int i = 0; i < 600 && ota_pending_verify(); i++) {
        if (i == 0) ESP_LOGW(TAG, "Restart waits until the new firmware is confirmed (a restart now would undo it)");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_restart();
}

void ota_restart_when_safe(void)
{
    if (xTaskCreate(restart_task, "rst", 3072, NULL, 5, NULL) != pdPASS) esp_restart();
}

static void ota_task(void *arg)
{
    // "Running ..." once the USB Serial/JTAG port is back after the reset (a PC monitor misses the first ~2.5 s):
    // the line every test checks first (docs/PROTOCOL.md §3)
    int64_t up_ms = esp_timer_get_time() / 1000;
    if (up_ms < 4000) vTaskDelay(pdMS_TO_TICKS(4000 - up_ms));
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running %s from %s, channel %s%s", st.current, run ? run->label : "?", st.channel,
             ota_pending_verify() ? " (new: not confirmed yet)" : "");
    TickType_t started = xTaskGetTickCount();
    bool validated = !ota_pending_verify(), was_online = false;
    TickType_t next_check = started + pdMS_TO_TICKS(60 * 1000);          // first check a minute after boot
    while (1) {
        TickType_t up = xTaskGetTickCount() - started;
        bool online = net_is_connected();
        if (!validated && up >= pdMS_TO_TICKS(VALID_AFTER_S * 1000) &&
            (online || up >= pdMS_TO_TICKS(VALID_OFFLINE_S * 1000))) {
            validated = true;
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "New firmware ran %lu s%s: marked valid (no rollback)", (unsigned long)(up / configTICK_RATE_HZ),
                     online ? " with Wi-Fi" : " (no Wi-Fi)");
        }
        if (want_install) {
            want_install = false;
            if (st.state == OTA_AVAILABLE && online) install();
        }
        if (retry_check) {                                // a failed download: check again in RETRY_FAILED_S
            retry_check = false;
            next_check = xTaskGetTickCount() + pdMS_TO_TICKS(RETRY_FAILED_S * 1000);
        }
        // Wi-Fi back after an outage, or not up for the first check: check now (the next one was up to 6 h away)
        if (online && !was_online && up >= pdMS_TO_TICKS(60 * 1000) && st.state != OTA_DOWNLOADING) want_check = true;
        was_online = online;
        if (want_check || (int32_t)(xTaskGetTickCount() - next_check) >= 0) {
            want_check = false;
            next_check = xTaskGetTickCount() + pdMS_TO_TICKS(CHECK_EVERY_S * 1000LL);
            if (online) { ESP_LOGI(TAG, "checking %s", st.channel); check(); }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
}

// The bootloader undid an update (it restarted before it was confirmed): say so once (log, update screen, page)
static void note_rollback(void)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (!bad || esp_ota_get_partition_description(bad, &d) != ESP_OK) return;
    nvs_handle_t h;
    char seen[32] = "";
    size_t n = sizeof(seen);
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_str(h, "rb_seen", seen, &n);
    if (strcmp(seen, d.version)) {
        ESP_LOGW(TAG, "%s was rolled back: it restarted before it was confirmed (running %s)", d.version, st.current);
        strlcpy(st.rolled_back, d.version, sizeof(st.rolled_back));
        nvs_set_str(h, "rb_seen", d.version);
        nvs_commit(h);
    }
    nvs_close(h);
}

static void probe_url(char *url, size_t n) { snprintf(url, n, OTA_SITE "channels.json"); }

void ota_start(ota_listener_t l)
{
    listener = l;
    svc_updates = svc_add(SVC_NAME_UPDATES, "updates", probe_url);
    net_set_restart(ota_restart_when_safe);
    ota_web_routes();
    mux = xSemaphoreCreateMutex();
    strlcpy(st.current, esp_app_get_description()->version, sizeof(st.current));
    strcpy(st.channel, "stable");
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(st.channel);
        nvs_get_str(h, "channel", st.channel, &n);
        nvs_close(h);
    }
    note_rollback();
    xTaskCreatePinnedToCore(ota_task, "ota", 8192, NULL, 2, &task, 0);   // install = TLS + flash writes (IDF examples: 8 KB)
}

void ota_check_now(void)
{
    want_check = true;
    if (task) xTaskNotifyGive(task);
}

bool ota_install(void)
{
    if (st.state != OTA_AVAILABLE) return false;
    want_install = true;
    if (task) xTaskNotifyGive(task);
    return true;
}

void ota_set_channel(const char *channel)
{
    if (strcmp(channel, "stable") && strcmp(channel, "beta")) return;
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(st.channel, channel, sizeof(st.channel));
    xSemaphoreGive(mux);
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, "channel", channel) != ESP_OK || nvs_commit(h) != ESP_OK) ESP_LOGE(TAG, "channel NOT saved");
        nvs_close(h);
    }
    publish();                                     // the app's screens show the channel (no check runs offline)
    ota_check_now();
}

void ota_get_status(ota_status_t *out)
{
    if (!mux) { memset(out, 0, sizeof(*out)); return; }   // before ota_start()
    xSemaphoreTake(mux, portMAX_DELAY);
    *out = st;
    xSemaphoreGive(mux);
}
