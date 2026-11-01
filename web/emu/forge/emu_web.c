// The display's settings page, under the emulator (index.html, an iframe of build/settings.html): its
// fetch('/api/...') is queued by the page (emu-settings.js) and served here, between LVGL frames as the display's web
// server task would, by the display's own route handlers: the app's (web_add_routes) and forge_ota's ota_web.c.
// forge_net's built-in routes are answered here: /api/info as web.c's (with the app's fields, web_set_info), and the
// ones that need the radio (Wi-Fi scan and save) say they need the real display; /api/snapshot isn't served. No key
// check: nothing else can reach this "display". A handler may wait (vTaskDelay, a semaphore a task gives): it runs in
// the main loop, never inside a call from JavaScript (ASYNCIFY can't unwind one made while the loop is suspended).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "web.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "net.h"
#include "forge_i18n.h"

#define MAX_TABLES 4
static const web_route_t *tables[MAX_TABLES];
static int sizes[MAX_TABLES], ntables;

void web_add_routes(const web_route_t *routes, int n)
{
    if (ntables < MAX_TABLES) { tables[ntables] = routes; sizes[ntables++] = n; }
}
void web_set_page(const uint8_t *start, const uint8_t *end) { (void)start; (void)end; }   // (build/settings.html)
void web_set_snapshot(web_snapshot_fn take, web_snapshot_free_fn release) { (void)take; (void)release; }
static web_info_fn info_fn;
void web_set_info(web_info_fn fn) { info_fn = fn; }
bool web_from_setup_ap(httpd_req_t *req) { (void)req; return false; }

// The page the firmware embeds; here the page is build/settings.html (main.c passes them to web_set_page)
const uint8_t index_html_start[] asm("_binary_index_html_start") = "";
const uint8_t index_html_end[] asm("_binary_index_html_end") = "";

const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t d;
    if (!d.version[0]) {
        snprintf(d.version, sizeof(d.version), "%s", EMU_VERSION);
        snprintf(d.project_name, sizeof(d.project_name), "%s", EMU_APP);
    }
    return &d;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *s)
{
    free(req->reply);
    req->reply = strdup(s ? s : "");
    if (!req->status) req->status = 200;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    free(req->reply);
    req->reply = strdup(msg ? msg : "");
    req->status = code;
    return ESP_OK;
}

esp_err_t web_send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    esp_err_t e = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return e;
}

cJSON *web_read_json(httpd_req_t *req) { return req->body && req->body[0] ? cJSON_Parse(req->body) : NULL; }

/* ---------- forge_net's built-in routes ---------- */
// GET /api/info, as web.c's (the page reads the language and the version from it)
static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    char ip[20] = "", ssid[NET_SSID_MAX + 1] = "";
    net_get_ip(ip, sizeof(ip));
    net_get_ssid(ssid, sizeof(ssid));
    wifi_ap_record_t ap;
    cJSON_AddStringToObject(j, "app", esp_app_get_description()->project_name);
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddStringToObject(j, "ssid", ssid);
    cJSON_AddNumberToObject(j, "rssi", esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0);
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(j, "setup", false);
    cJSON_AddStringToObject(j, "lang", i18n_code(i18n_lang()));
    cJSON *langs = cJSON_AddArrayToObject(j, "languages");
    for (int i = 0; i < i18n_count(); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "code", i18n_code(i));
        cJSON_AddStringToObject(o, "name", i18n_name(i));
        cJSON_AddItemToArray(langs, o);
    }
    if (info_fn) info_fn(j);
    return web_send_json(req, j);
}
static esp_err_t scan_get(httpd_req_t *req) { return httpd_resp_sendstr(req, "[]"); }
static esp_err_t wifi_post(httpd_req_t *req)
{
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wi-Fi setup needs the real display");
}
static const web_route_t builtin[] = {
    { "/api/info", HTTP_GET, info_get },
    { "/api/scan", HTTP_GET, scan_get },
    { "/api/wifi", HTTP_POST, wifi_post, .keyed = true },
};

/* ---------- the page's queue (index.html: Module.emuApiQ, emuApi()) ---------- */

// The next request the page queued: its id (0: none), method, path (without the query) and body (malloc'd)
EM_JS(int, js_api_take, (char *method, int mn, char *path, int pn, char **body), {
    const q = Module.emuApiQ;
    if (!q || !q.length) return 0;
    const r = q.shift();
    stringToUTF8(r.method, method, mn);
    stringToUTF8(r.path.split('?')[0], path, pn);
    const b = new TextEncoder().encode(r.body || ""), p = _malloc(b.length + 1);
    HEAPU8.set(b, p);
    HEAPU8[p + b.length] = 0;
    HEAP32[body >> 2] = p;
    return r.id;
});

EM_JS(void, js_api_done, (int id, int status, const char *reply), {
    const done = Module.emuApiWait && Module.emuApiWait[id];
    if (!done) return;
    delete Module.emuApiWait[id];
    done({ status: status, body: UTF8ToString(reply) });
});

static const web_route_t *find(const char *path, int method)
{
    for (int i = 0; i < (int)(sizeof(builtin) / sizeof(builtin[0])); i++)
        if (builtin[i].method == method && !strcmp(builtin[i].uri, path)) return &builtin[i];
    for (int t = 0; t < ntables; t++)
        for (int i = 0; i < sizes[t]; i++)
            if (tables[t][i].method == method && !strcmp(tables[t][i].uri, path)) return &tables[t][i];
    return NULL;
}

// Serve what the page asked for since the last call (emu_loop.c, between LVGL frames)
void emu_web_poll(void)
{
    char method[8], path[96], *body = NULL;
    int id;
    while ((id = js_api_take(method, sizeof(method), path, sizeof(path), &body))) {
        httpd_req_t req = { .uri = path, .method = strcmp(method, "POST") ? HTTP_GET : HTTP_POST,
                            .body = body ? body : "", .content_len = body ? strlen(body) : 0 };
        const web_route_t *r = find(path, req.method);
        if (r) r->fn(&req);
        else httpd_resp_send_err(&req, HTTPD_404_NOT_FOUND, "not here");
        js_api_done(id, req.status ? req.status : 200, req.reply ? req.reply : "");
        free(req.reply);
        free(body);
        body = NULL;
    }
}
