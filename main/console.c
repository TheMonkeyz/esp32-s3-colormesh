// The app's test console commands (see console.h), registered with forge_core's testcon. Every answer is one log
// line with the tag "test": "test: ok ...", "test: <name> ..." or "test: error ..." (docs/PROTOCOL.md §2). The
// harness and the Pages site's "Configure over USB" card parse these lines: keep their shape.
#include "console.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "testcon.h"
#include "screens.h"
#include "config.h"
#include "mesh.h"
#include "mesh_fw.h"
#include "ui.h"

// driver/usb_serial_jtag.h: forge_core links the driver (testcon owns it) but only as a private dependency, and
// main/CMakeLists.txt does not list esp_driver_usb_serial_jtag; the one function needed, as the driver declares it
int usb_serial_jtag_write_bytes(const void *src, size_t size, TickType_t ticks_to_wait);
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks_to_wait);

static const char *TAG = "test";

static bool parse_onoff(int argc, char **argv, bool *on)
{
    if (argc != 2) return false;
    if (!strcmp(argv[1], "on")) { *on = true; return true; }
    if (!strcmp(argv[1], "off")) { *on = false; return true; }
    return false;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* ---------- settings ---------- */

static void cmd_color(int argc, char **argv)
{
    const char *s = argc == 2 ? argv[1] : "";
    if (*s == '#') s++;
    uint8_t v[3];
    bool ok = strlen(s) == 6;
    for (int i = 0; ok && i < 3; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) ok = false;
        else v[i] = hi << 4 | lo;
    }
    if (!ok) { ESP_LOGW(TAG, "error color: expected RRGGBB"); return; }
    config_set_color(v[0], v[1], v[2]);      // "cfg: color #rrggbb"
    mesh_set_color(v[0], v[1], v[2]);
    ESP_LOGI(TAG, "ok color #%02x%02x%02x", v[0], v[1], v[2]);   // the screens pick it up within 50 ms
}

static void cmd_devmode(int argc, char **argv)
{
    bool on;
    if (!parse_onoff(argc, argv, &on)) { ESP_LOGW(TAG, "error devmode: expected on|off"); return; }
    config_set_dev(on);
    mesh_set_dev_mode(on);
    ESP_LOGI(TAG, "ok dev %s", on ? "on" : "off");
}

static void cmd_name(int argc, char **argv)
{
    char name[64] = "";
    for (int i = 1; i < argc; i++) {         // the words again, one space between them
        if (i > 1) strlcat(name, " ", sizeof(name));
        strlcat(name, argv[i], sizeof(name));
    }
    if (strlen(name) > 16) { ESP_LOGW(TAG, "error name: 16 characters at most"); return; }
    config_set_name(name);
    ESP_LOGI(TAG, "ok name %s", config_get()->name);
}

static void cmd_bright(int argc, char **argv)
{
    char *end = NULL;
    long v = argc == 2 ? strtol(argv[1], &end, 10) : -1;
    if (argc != 2 || !end || *end || end == argv[1] || v < 0 || v > 100) {
        ESP_LOGW(TAG, "error bright: expected 0-100");
        return;
    }
    config_set_bright((uint8_t)v);           // applied at once
    ESP_LOGI(TAG, "ok bright %ld", v);
}

/* ---------- mesh ---------- */

EXT_RAM_BSS_ATTR static mesh_peer_t peers[16];   // static: the console task's stack is 4 KB

static void fmt_addr(const uint8_t a[6], char *out, int n)
{
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static void cmd_peers(int argc, char **argv)
{
    int n = mesh_peers(peers, 16);
    if (n < 0) n = 0;
    ESP_LOGI(TAG, "peers n=%d", n);
    for (int i = 0; i < n && i < 16; i++) {
        const mesh_peer_t *p = &peers[i];
        char addr[18], ver[24];
        fmt_addr(p->addr, addr, sizeof(addr));
        ui_fmt_ver(p->ver, ver, sizeof(ver));
        ESP_LOGI(TAG, "peer %s %s #%02x%02x%02x %s rssi=%d", addr, ver, p->r, p->g, p->b,
                 p->holding ? "hold" : "idle", p->rssi);
    }
}

// JSON string body: quotes and backslashes escaped, control characters dropped
static int json_str(char *out, int n, const char *s)
{
    int o = 0;
    for (; *s && o < n - 2; s++) {
        unsigned char c = *s;
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') out[o++] = '\\';
        out[o++] = c;
    }
    out[o] = 0;
    return o;
}

static void cmd_cfg(int argc, char **argv)
{
    EXT_RAM_BSS_ATTR static char js[1600];
    const app_cfg_t *c = config_get();
    char name[40];
    json_str(name, sizeof(name), c->name);
    int n = snprintf(js, sizeof(js),
                     "{\"color\":\"#%02x%02x%02x\",\"dev\":%s,\"name\":\"%s\",\"bright\":%u,\"version\":\"%s\","
                     "\"addr\":\"%s\",\"peers\":[",
                     c->r, c->g, c->b, c->dev_mode ? "true" : "false", name, c->bright,
                     esp_app_get_description()->version, mesh_own_addr());
    int np = mesh_peers(peers, 16);
    for (int i = 0; i < np && i < 16 && n < (int)sizeof(js) - 100; i++) {
        const mesh_peer_t *p = &peers[i];
        char addr[18], ver[24];
        fmt_addr(p->addr, addr, sizeof(addr));
        ui_fmt_ver(p->ver, ver, sizeof(ver));
        n += snprintf(js + n, sizeof(js) - n, "%s{\"addr\":\"%s\",\"color\":\"#%02x%02x%02x\",\"holding\":%s,\"ver\":\"%s\"}",
                      i ? "," : "", addr, p->r, p->g, p->b, p->holding ? "true" : "false", ver);
    }
    snprintf(js + n, sizeof(js) - n, "]}");
    ESP_LOGI(TAG, "cfg %s", js);
}

static void cmd_hold(int argc, char **argv)
{
    bool on;
    if (!parse_onoff(argc, argv, &on)) { ESP_LOGW(TAG, "error hold: expected on|off"); return; }
    mesh_set_holding(on);                    // "mesh: hold on|off"
    ESP_LOGI(TAG, "ok hold %s", on ? "on" : "off");
}

// "9a", or a whole address "aa:bb:cc:dd:ee:9a": the origin id is the address's last (least significant) byte
static void cmd_fwpull(int argc, char **argv)
{
    const char *s = argc == 2 ? argv[1] : "";
    const char *last = strrchr(s, ':');
    if (last) {
        if (strlen(s) != 17) s = "";
        else s = last + 1;
    }
    int len = strlen(s), v = 0;
    bool ok = len >= 1 && len <= 2;
    for (int i = 0; ok && i < len; i++) {
        int h = hexval(s[i]);
        if (h < 0) ok = false;
        else v = v << 4 | h;
    }
    if (!ok) { ESP_LOGW(TAG, "error fwpull: expected an address or its last byte (9a)"); return; }
    mesh_fw_set_target((uint8_t)v);
    ESP_LOGI(TAG, "ok fwpull %02x", v);
}

/* ---------- shot ----------
 * "shot [screen] [div]": a screen (the registry's names; default "shown", the one on the display) sent over USB as
 * base64 RGB565, every div-th pixel of every div-th row (div 1..4, default 2): "SHOT name w h", one "S:" line per
 * row, "SHOTEND". tools/shot.py turns a log into PNGs. From esp32-s3-meteobus (port/esp32-p4, main/console.c).
 * Straight to the USB driver (testcon's): through the log it would take far longer. */
static void usb_line(const char *s, size_t n)
{
    for (size_t o = 0; o < n;) {
        size_t k = n - o > 128 ? 128 : n - o;             // the driver's ring buffer (testcon: 256 B) refuses bigger writes
        int w = usb_serial_jtag_write_bytes(s + o, k, pdMS_TO_TICKS(500));
        if (w <= 0) return;                               // no host reading: drop the rest
        o += w;
    }
}

// In its own task: rendering a screen needs more stack than the console task has
static char shot_name[24];
static int shot_div;
static volatile bool shot_busy;

static void shot_task(void *arg)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char name[24];
    strlcpy(name, shot_name, sizeof(name));
    int div = shot_div;
    if (!strcmp(name, "shown")) {
        const char *cur = screens_current();
        if (strcmp(cur, "other") && strcmp(cur, "busy")) strlcpy(name, cur, sizeof(name));
    }
    web_image_t img = { 0 };
    bool got = screens_snapshot(strcmp(name, "shown") ? name : "current", &img);
    if (!got || !img.data) {
        ESP_LOGW(TAG, "error shot %s: no picture", name);
        if (got) screens_snapshot_free(&img);
        shot_busy = false;
        vTaskDelete(NULL);
        return;
    }
    int w = img.w / div, h = img.h / div;
    uint16_t *row = heap_caps_malloc(w * 2, MALLOC_CAP_SPIRAM);
    char *line = heap_caps_malloc(w * 2 * 4 / 3 + 16, MALLOC_CAP_SPIRAM);
    if (row && line) {
        char hd[64];
        int n = snprintf(hd, sizeof(hd), "\nSHOT %s %d %d\n", name, w, h);
        usb_line(hd, n);
        for (int y = 0; y < h; y++) {
            const uint16_t *src = (const uint16_t *)(img.data + (size_t)y * div * img.stride);
            for (int x = 0; x < w; x++) row[x] = src[x * div];
            const uint8_t *b = (const uint8_t *)row;
            int len = w * 2, o = 0;
            line[o++] = 'S'; line[o++] = ':';
            for (int i = 0; i < len; i += 3) {
                uint32_t v = b[i] << 16 | (i + 1 < len ? b[i + 1] << 8 : 0) | (i + 2 < len ? b[i + 2] : 0);
                line[o++] = b64[v >> 18 & 63];
                line[o++] = b64[v >> 12 & 63];
                line[o++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
                line[o++] = i + 2 < len ? b64[v & 63] : '=';
            }
            line[o++] = '\n';
            usb_line(line, o);
        }
        usb_line("SHOTEND\n", 8);
        // The log writes to the USB port's FIFO directly, beside the driver: a log line sent while the driver still
        // emptied its buffer was lost (the "ok shot" line never arrived). Let the driver finish first.
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    free(row);
    free(line);
    screens_snapshot_free(&img);                          // lv_draw_buf_destroy under the display lock
    if (row && line) ESP_LOGI(TAG, "ok shot %s %dx%d", name, w, h);
    else ESP_LOGW(TAG, "error shot %s: no memory", name);
    shot_busy = false;
    vTaskDelete(NULL);
}

static void cmd_shot(int argc, char **argv)
{
    if (shot_busy) { ESP_LOGW(TAG, "error shot: one is being sent"); return; }
    strlcpy(shot_name, argc > 1 ? argv[1] : "shown", sizeof(shot_name));
    shot_div = argc > 2 ? atoi(argv[2]) : 2;
    if (shot_div < 1 || shot_div > 4) shot_div = 2;
    shot_busy = true;
    if (xTaskCreatePinnedToCore(shot_task, "shot", 16384, NULL, 2, NULL, 0) != pdPASS) {
        shot_busy = false;
        ESP_LOGW(TAG, "error shot %s: no task", shot_name);
    }
}

void console_init(void)
{
    testcon_register("color", "color RRGGBB", cmd_color);
    testcon_register("devmode", "devmode on|off", cmd_devmode);
    testcon_register("name", "name TEXT", cmd_name);
    testcon_register("bright", "bright 0-100", cmd_bright);
    testcon_register("peers", "peers", cmd_peers);
    testcon_register("cfg", "cfg", cmd_cfg);
    testcon_register("hold", "hold on|off", cmd_hold);
    testcon_register("shot", "shot [screen] [div]", cmd_shot);
    testcon_register("fwpull", "fwpull ADDR|ORIGIN", cmd_fwpull);
}
