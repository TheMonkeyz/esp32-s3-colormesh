#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "cJSON.h"

// The settings page and its API (docs/PROTOCOL.md §4). HTTPS on 443 (CONFIG_FORGE_HTTPS_PORT) with this device's own
// certificate (tlscert) on the home network; plain HTTP on 80 is the captive portal on the setup network and only redirects to HTTPS on the
// home network. Every change (POST) and every snapshot carries the device's key in X-Key (see "Who may change
// things" in web.c). Built in: GET / (the app's main/web/index.html), /api/info, /api/scan, POST /api/wifi,
// /api/snapshot. Components and the app add routes with web_add_routes() before web_start().

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*fn)(httpd_req_t *req);
    bool keyed;         // a change or a snapshot: JSON (POST) and the key, except on the setup network
    bool not_ap;        // never on the setup network
} web_route_t;
void web_add_routes(const web_route_t *routes, int n);   // the array must stay valid (static const)

// /api/snapshot?screen=<name>: an RGB565 picture of a screen rendered off-display (forge_lvgl's screens_snapshot)
typedef struct { const uint8_t *data; int w, h, stride; void *priv; } web_image_t;
typedef bool (*web_snapshot_fn)(const char *screen, web_image_t *out);
typedef void (*web_snapshot_free_fn)(web_image_t *img);
void web_set_snapshot(web_snapshot_fn take, web_snapshot_free_fn release);

typedef void (*web_info_fn)(cJSON *info);   // adds the app's fields to GET /api/info
void web_set_info(web_info_fn fn);

// The page served at / : the app embeds main/web/index.html (EMBED_TXTFILES) and passes its symbols, e.g.
//   extern const uint8_t page_start[] asm("_binary_index_html_start"), page_end[] asm("_binary_index_html_end");
void web_set_page(const uint8_t *start, const uint8_t *end);
void web_start(void);
const char *web_port(void);            // "" on 443, else ":<port>" (CONFIG_FORGE_HTTPS_PORT): after the host in URLs
const char *web_key(void);             // the key a change must carry (X-Key); goes in the settings QR code (#k=)
bool web_url(char *out, int n);        // "https://<ip>/#k=<key>" for the settings QR code; false without an address

// Helpers for route handlers
esp_err_t web_send_json(httpd_req_t *req, cJSON *j);   // sends and deletes j
cJSON *web_read_json(httpd_req_t *req);                // the body (at most 1 KB), NULL if missing or not JSON
bool web_from_setup_ap(httpd_req_t *req);               // the request came in on the setup network (192.168.4.x)
