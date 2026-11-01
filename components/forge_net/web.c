// Settings page and API (see web.h). HTTPS on 443 (CONFIG_FORGE_HTTPS_PORT) on the home network (a phone's GPS, and
// the key, need a secure page); plain HTTP on 80 is the captive portal on the setup network and only redirects to
// HTTPS on the home network.
#include "sdkconfig.h"
#include "web.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_https_server.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "tlscert.h"
#include "net.h"
#include "testcon.h"
#include "forge_i18n.h"

static const char *TAG = "web";

static const uint8_t *page_start, *page_end;
static web_snapshot_fn snap_take;
static web_snapshot_free_fn snap_free;
static web_info_fn info_fn;

void web_set_page(const uint8_t *start, const uint8_t *end) { page_start = start; page_end = end; }
void web_set_snapshot(web_snapshot_fn take, web_snapshot_free_fn release) { snap_take = take; snap_free = release; }
void web_set_info(web_info_fn fn) { info_fn = fn; }

esp_err_t web_send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = s ? httpd_resp_sendstr(req, s) : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    free(s);
    cJSON_Delete(j);
    return r;
}

cJSON *web_read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) return NULL;
    char *buf = calloc(1, req->content_len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

// The page in 1 KB chunks: small TLS records are more reliable on the ESP32 (a whole page in one send arrived
// truncated when internal DMA memory was short, and the buttons "did nothing")
static esp_err_t index_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET / (page), free internal %u, largest DMA block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!page_start) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no page");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char *p = (const char *)page_start;
    size_t left = page_end - page_start - 1;            // EMBED_TXTFILES adds a NUL
    while (left) {
        size_t n = left > 1024 ? 1024 : left;
        if (httpd_resp_send_chunk(req, p, n) != ESP_OK) { ESP_LOGW(TAG, "page send failed"); return ESP_FAIL; }
        p += n; left -= n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// {"app","version","ip","ssid","rssi","uptime_s","setup","lang","languages":[{code,name}]} + the app's fields
static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    char ip[20] = "", ssid[NET_SSID_MAX + 1] = "";
    // On the setup network (anyone with its password, during an outage too): not the home network's name or address
    // (weather_amoled's rule since v1.12.0; espforge gave both, found aligning the two, October 4)
    bool setup = web_from_setup_ap(req);
    if (!setup) {
        net_get_ip(ip, sizeof(ip));
        net_get_ssid(ssid, sizeof(ssid));
    }
    wifi_ap_record_t ap;
    cJSON_AddStringToObject(j, "app", esp_app_get_description()->project_name);
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddStringToObject(j, "ssid", ssid);
    cJSON_AddNumberToObject(j, "rssi", esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0);
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(j, "setup", setup);
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

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

// [{"ssid":"...","rssi":-52,"secure":true}, ...] strongest first, one entry per name
static esp_err_t scan_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /api/scan");
    cJSON *arr = cJSON_CreateArray();
    uint16_t n = 30;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    wifi_scan_config_t sc = { .show_hidden = false };
    if (recs && esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        qsort(recs, n, sizeof(recs[0]), by_rssi);
        for (int i = 0; i < n; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            if (!ssid[0] || !strcmp(ssid, SETUP_AP_SSID)) continue;
            bool dup = false;
            for (int k = 0; k < i && !dup; k++) dup = !strcmp((const char *)recs[k].ssid, ssid);
            if (dup) continue;                               // already listed with a stronger signal
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", ssid);
            cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
            cJSON_AddBoolToObject(o, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, o);
        }
        ESP_LOGI(TAG, "scan: %d networks", cJSON_GetArraySize(arr));
    } else {
        ESP_LOGW(TAG, "scan failed");
    }
    free(recs);
    return web_send_json(req, arr);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    cJSON *ssid = j ? cJSON_GetObjectItem(j, "ssid") : NULL;
    cJSON *pass = j ? cJSON_GetObjectItem(j, "pass") : NULL;
    const char *p = cJSON_IsString(pass) ? pass->valuestring : "";
    bool ok = cJSON_IsString(ssid) && net_creds_valid(ssid->valuestring, p) && net_save_creds(ssid->valuestring, p);
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad wifi");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    net_restart();                                         // after the answer; forge_ota: not in an update's first minute
    return ESP_OK;
}

// GET /api/snapshot?screen=<name>: the screen rendered off-display, as a 24-bit BMP (tools/snapshot.py), sent in
// 16-row pieces from PSRAM
static esp_err_t snapshot_get(httpd_req_t *req)
{
    char q[48], name[16] = "current";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "screen", name, sizeof(name));
    web_image_t img = {0};
    if (!snap_take || !snap_take(name, &img)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such screen");
    uint8_t *buf = heap_caps_malloc(16 * (img.w * 3 + 3), MALLOC_CAP_SPIRAM);
    if (!buf) {
        snap_free(&img);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");
    }
    int w = img.w, h = img.h, row = (w * 3 + 3) & ~3;
    uint32_t size = 54 + row * h;
    uint8_t hd[54] = { 'B', 'M' };
    #define LE32(o, v) do { uint32_t _v = (v); hd[o] = _v; hd[o + 1] = _v >> 8; hd[o + 2] = _v >> 16; hd[o + 3] = _v >> 24; } while (0)
    LE32(2, size); LE32(10, 54); LE32(14, 40); LE32(18, w); LE32(22, (uint32_t)-h);   // negative height: top-down
    hd[26] = 1; hd[28] = 24; LE32(34, row * h);
    #undef LE32
    httpd_resp_set_type(req, "image/bmp");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)hd, sizeof(hd));
    for (int y0 = 0; y0 < h && err == ESP_OK; y0 += 16) {
        int n = h - y0 < 16 ? h - y0 : 16;
        memset(buf, 0, n * row);
        for (int y = 0; y < n; y++) {
            const uint16_t *src = (const uint16_t *)(img.data + (y0 + y) * img.stride);
            uint8_t *d = buf + y * row;
            for (int x = 0; x < w; x++, d += 3) {
                uint16_t p = src[x];
                d[0] = (p & 0x1F) << 3; d[1] = (p >> 5 & 0x3F) << 2; d[2] = (p >> 11) << 3;   // B, G, R
            }
        }
        err = httpd_resp_send_chunk(req, (const char *)buf, n * row);
    }
    free(buf);
    snap_free(&img);
    ESP_LOGI(TAG, "snapshot %s %dx%d %s, stack %u B spare", name, w, h, err == ESP_OK ? "sent" : "failed",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : ESP_FAIL;
}

// True when the request came in on the setup access point (192.168.4.x) rather than the home network
bool web_from_setup_ap(httpd_req_t *req)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int fd = httpd_req_to_sockfd(req);
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return false;
    if (ss.ss_family == AF_INET) {
        uint32_t a = ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
        return (a & 0xFFFFFF00) == 0xC0A80400;          // 192.168.4.0/24
    }
    if (ss.ss_family == AF_INET6) {                      // IPv4-mapped (::ffff:192.168.4.x)
        const uint8_t *b = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        return b[10] == 0xFF && b[11] == 0xFF && b[12] == 192 && b[13] == 168 && b[14] == 4;
    }
    return false;
}

static esp_err_t redirect_to(httpd_req_t *req, const char *loc)
{
    char body[96];
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_type(req, "text/html");
    // iOS needs a body to recognise the captive portal
    snprintf(body, sizeof(body), "<html><body><a href=\"/\">%s setup</a></body></html>",
             CONFIG_FORGE_PORTAL_NAME[0] ? CONFIG_FORGE_PORTAL_NAME : esp_app_get_description()->project_name);
    return httpd_resp_sendstr(req, body);
}

// Plain HTTP ":80"
//  - on the setup AP: this *is* the captive portal. Serve the page over HTTP (the phone's sign-in browser
//    rejects a self-signed certificate) and send every other URL (OS connectivity checks) to it.
//  - on the home network: move to HTTPS.
static esp_err_t http_root_get(httpd_req_t *req)
{
    if (web_from_setup_ap(req)) return index_get(req);
    char ip[20] = "", loc[40];
    if (!net_get_ip(ip, sizeof(ip))) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no address yet");
    snprintf(loc, sizeof(loc), "https://%s%s/", ip, web_port());   // the device's own address, never the Host header
    return redirect_to(req, loc);
}

// Test console only ("portal windows-quiet"), until restart: answer Windows' connectivity check as if online, so
// the test PC joining the setup network doesn't pop a browser tab. Phones and other PCs still get the portal.
static volatile bool windows_quiet;

static esp_err_t http_other_get(httpd_req_t *req)
{
    if (web_from_setup_ap(req)) {
        if (windows_quiet && !strcmp(req->uri, "/connecttest.txt")) {
            ESP_LOGI(TAG, "captive: Windows check answered (test console)");
            httpd_resp_set_type(req, "text/plain");
            return httpd_resp_sendstr(req, "Microsoft Connect Test");
        }
        ESP_LOGI(TAG, "captive: %.60s -> portal", req->uri);
        return redirect_to(req, "http://192.168.4.1/");
    }
    return http_root_get(req);
}

/* ---------- Who may change things ----------
 *  - every /api request must name the device itself in Host (a DNS-rebinding name is refused: 421);
 *  - on the home network the API is HTTPS only (port 80: GET -> 302 to the HTTPS page, POST -> 403);
 *  - a change (POST) must be JSON (415) and carry the device's key in X-Key (401), as must a snapshot. The key is
 *    random, made at the first start (NVS "web"/"key"), and travels in the settings QR code on the display
 *    (https://<ip>/#k=<key>: a fragment, never sent to a server); the page keeps it. Scanning the code proves you
 *    can see the display. A custom header also makes a browser ask first (CORS preflight), which this server never
 *    answers: another site's page can't send one.
 *  - on the setup network no key is needed: its password is shown on the display too.
 *  - the test console prints the key ("key"): USB access means someone at the device. */
#define KEY_LEN 16
static char key[KEY_LEN + 1];

static void key_init(void)
{
    nvs_handle_t h;
    size_t n = sizeof(key);
    bool open = nvs_open("web", NVS_READWRITE, &h) == ESP_OK;
    if (open && nvs_get_str(h, "key", key, &n) == ESP_OK && strlen(key) == KEY_LEN) { nvs_close(h); return; }
    uint8_t r[KEY_LEN / 2];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < KEY_LEN / 2; i++) snprintf(key + 2 * i, 3, "%02x", r[i]);
    bool saved = open && nvs_set_str(h, "key", key) == ESP_OK && nvs_commit(h) == ESP_OK;
    if (open) nvs_close(h);
    ESP_LOGI(TAG, "new settings key%s", saved ? "" : " (not saved: a new one after a restart)");
}

const char *web_key(void) { return key; }

bool web_url(char *out, int n)
{
    char ip[20];
    if (!net_get_ip(ip, sizeof(ip))) return false;
    snprintf(out, n, "https://%s%s/#k=%s", ip, web_port(), key);
    return true;
}

static bool own_host(httpd_req_t *req)
{
    char host[64] = "", ip[20];
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;                               // a port
    if (web_from_setup_ap(req)) return !strcmp(host, "192.168.4.1");
    return net_get_ip(ip, sizeof(ip)) && !strcmp(host, ip);
}

static esp_err_t refuse(httpd_req_t *req, const char *status, const char *why)
{
    ESP_LOGW(TAG, "%s %.40s refused: %s", req->method == HTTP_POST ? "POST" : "GET", req->uri, status);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char b[64];
    snprintf(b, sizeof(b), "{\"error\":\"%s\"}", why);
    return httpd_resp_sendstr(req, b);
}

typedef struct { const web_route_t *r; bool plain; } bound_t;   // plain: the port-80 copy

static esp_err_t guarded(httpd_req_t *req)
{
    const bound_t *b = req->user_ctx;
    const web_route_t *r = b->r;
    bool ap = web_from_setup_ap(req);
    if (b->plain && !ap) {                               // home network over plain HTTP: HTTPS only
        if (req->method == HTTP_POST) return refuse(req, "403 Forbidden", "https");
        char ip[20], loc[40];
        if (!net_get_ip(ip, sizeof(ip))) return refuse(req, "403 Forbidden", "https");
        snprintf(loc, sizeof(loc), "https://%s%s/", ip, web_port());
        return redirect_to(req, loc);
    }
    if (r->not_ap && ap) return refuse(req, "403 Forbidden", "setup");
    if (!own_host(req)) return refuse(req, "421 Misdirected Request", "host");
    if (r->keyed && req->method == HTTP_POST) {
        char ct[48] = "";
        httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
        if (strncmp(ct, "application/json", 16)) return refuse(req, "415 Unsupported Media Type", "json");
    }
    if (r->keyed && !ap) {
        char k[KEY_LEN + 2] = "";
        httpd_req_get_hdr_value_str(req, "X-Key", k, sizeof(k));
        uint8_t diff = strlen(k) != KEY_LEN;
        for (int i = 0; i < KEY_LEN; i++) diff |= k[i] ^ key[i];      // same time whatever matches
        if (diff) return refuse(req, "401 Unauthorized", "key");
    }
    return r->fn(req);
}

static const web_route_t builtin[] = {
    { "/api/info",     HTTP_GET,  info_get, .keyed = false },
    { "/api/scan",     HTTP_GET,  scan_get, .keyed = false },
    { "/api/wifi",     HTTP_POST, wifi_post, .keyed = true },
    { "/api/snapshot", HTTP_GET,  snapshot_get, .keyed = true, .not_ap = true },
};

#define MAX_ROUTES 32
EXT_RAM_BSS_ATTR static const web_route_t *routes[MAX_ROUTES];   // (PSRAM: L185)
static int nroutes;

void web_add_routes(const web_route_t *r, int n)
{
    for (int i = 0; i < n; i++) {
        if (nroutes == MAX_ROUTES) { ESP_LOGE(TAG, "no room for route %s (MAX_ROUTES)", r[i].uri); return; }
        routes[nroutes++] = &r[i];
    }
}

static void add_all(httpd_handle_t s, bool plain)
{
    bound_t *b = calloc(nroutes, sizeof(bound_t));        // kept for the server's lifetime
    for (int i = 0; b && i < nroutes; i++) {
        b[i] = (bound_t){ routes[i], plain };
        httpd_uri_t u = { .uri = routes[i]->uri, .method = routes[i]->method, .handler = guarded, .user_ctx = &b[i] };
        if (httpd_register_uri_handler(s, &u) != ESP_OK) ESP_LOGE(TAG, "route %s not registered", routes[i]->uri);
    }
}

static void start_https(void)
{
    const char *cert, *pkey;
    size_t cert_len, key_len;
    if (!tlscert_get(&cert, &cert_len, &pkey, &key_len)) { ESP_LOGE(TAG, "no TLS certificate, HTTPS disabled"); return; }
    tlscert_log_fingerprint();
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.port_secure = CONFIG_FORGE_HTTPS_PORT;                // 443 unless the board needs another (Kconfig)
    conf.servercert = (const uint8_t *)cert;
    conf.servercert_len = cert_len;
    conf.prvtkey_pem = (const uint8_t *)pkey;
    conf.prvtkey_len = key_len;
    conf.httpd.max_uri_handlers = nroutes + 2;
    conf.httpd.stack_size = 10240;     // TLS handshake ~3.3 KB, and GET /api/snapshot renders a whole screen here
                                       // ("web: snapshot ... stack N B spare" in the log)
    conf.httpd.max_open_sockets = 5;
    conf.httpd.lru_purge_enable = true;
    httpd_handle_t s = NULL;
    if (httpd_ssl_start(&s, &conf) != ESP_OK) { ESP_LOGE(TAG, "HTTPS server failed to start"); return; }
    httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = index_get };
    httpd_register_uri_handler(s, &page);
    add_all(s, false);
}

const char *web_port(void)
{
    static char p[8];
    if (CONFIG_FORGE_HTTPS_PORT != 443 && !p[0]) snprintf(p, sizeof(p), ":%d", CONFIG_FORGE_HTTPS_PORT);
    return p;
}

static void cmd_key(int argc, char **argv) { ESP_LOGI("test", "key %s", key); }

static void cmd_portal(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "windows-quiet")) {
        windows_quiet = true;                              // until restart
        ESP_LOGI("test", "ok portal windows-quiet");
    } else {
        ESP_LOGW("test", "error portal: windows-quiet");
    }
}

void web_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    key_init();
    testcon_register("key", "key", cmd_key);
    testcon_register("portal", "portal windows-quiet", cmd_portal);
    web_add_routes(builtin, sizeof(builtin) / sizeof(builtin[0]));

    start_https();

    // The control port: HTTPS's default is 32768 + 1 = 32769 already; two servers on one control port fail
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.max_uri_handlers = nroutes + 2;
    hc.max_open_sockets = 6;
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;              // idle ~1.2 KB; serving the portal's page left 1144 B of 4096
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &hc) == ESP_OK) {
        // Specific routes first; the wildcard catches everything else. The API is served here only to the setup
        // network (the captive portal's page); the home network is redirected to HTTPS (guarded()).
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = http_root_get };
        httpd_register_uri_handler(h, &root);
        add_all(h, true);
        httpd_uri_t other = { .uri = "/*", .method = HTTP_GET, .handler = http_other_get };
        httpd_register_uri_handler(h, &other);
    }
    char ip[20];
    if (net_get_ip(ip, sizeof(ip))) ESP_LOGI(TAG, "Settings page: https://%s%s/", ip, web_port());
    else ESP_LOGI(TAG, "Web servers started (no IP yet)");
}
