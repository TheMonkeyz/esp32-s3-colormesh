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
#include "esp_timer.h"
#include "testcon.h"
#include "screens.h"
#include "board.h"
#include "config.h"
#include "fw_auth.h"
#include "mesh.h"
#include "mesh_fw.h"
#include "ui.h"
#include "wave.h"

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

// "RRGGBB" or "#RRGGBB" into v
static bool parse_rgb(const char *s, uint8_t v[3])
{
    if (*s == '#') s++;
    if (strlen(s) != 6) return false;
    for (int i = 0; i < 3; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        v[i] = hi << 4 | lo;
    }
    return true;
}

static void cmd_color(int argc, char **argv)
{
    uint8_t v[3];
    if (!parse_rgb(argc == 2 ? argv[1] : "", v)) { ESP_LOGW(TAG, "error color: expected RRGGBB"); return; }
    config_set_color(v[0], v[1], v[2]);      // "cfg: color #rrggbb"
    mesh_set_color(v[0], v[1], v[2]);
    ESP_LOGI(TAG, "ok color #%02x%02x%02x", v[0], v[1], v[2]);   // the screens pick it up within 50 ms
}

// "color2 RRGGBB": the waves' colour B; "color2 auto": back to the opposite hue of the colour. Pure black is kept as
// #010101 (the advert's 0, 0, 0 means automatic): the answer says which colour was set
static void cmd_color2(int argc, char **argv)
{
    uint8_t v[3];
    if (argc == 2 && !strcmp(argv[1], "auto")) {
        config_clear_color2();               // "cfg: color2 auto"
        mesh_set_color2(0, 0, 0, false);
        ESP_LOGI(TAG, "ok color2 auto");
        return;
    }
    if (!parse_rgb(argc == 2 ? argv[1] : "", v)) { ESP_LOGW(TAG, "error color2: expected RRGGBB or auto"); return; }
    config_set_color2(v[0], v[1], v[2]);     // "cfg: color2 #rrggbb"
    const app_cfg_t *c = config_get();
    mesh_set_color2(c->b2_r, c->b2_g, c->b2_b, true);
    ESP_LOGI(TAG, "ok color2 #%02x%02x%02x", c->b2_r, c->b2_g, c->b2_b);   // the ring's mark follows within 50 ms
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
    char name[40], c2[8] = "auto";
    json_str(name, sizeof(name), c->name);
    if (c->b2_set) snprintf(c2, sizeof(c2), "#%02x%02x%02x", c->b2_r, c->b2_g, c->b2_b);
    // Secure updates: "admin_key" is this board's public key while it is the admin, "trusted" the short id of the
    // admin key it trusts (the first 8 base64 characters)
    char akey[FWA_B64_PUB_LEN + 3] = "null", tkey[FWA_SHORT_LEN + 3] = "null";
    if (c->admin && c->has_admin_key) {
        akey[0] = '"';
        fwa_b64_encode(c->admin_pub, FWA_PUB_LEN, akey + 1, sizeof(akey) - 2);
        strlcat(akey, "\"", sizeof(akey));
    }
    if (c->has_trusted) {
        tkey[0] = '"';
        fwa_short_id(c->trusted, tkey + 1);
        strlcat(tkey, "\"", sizeof(tkey));
    }
    int n = snprintf(js, sizeof(js),
                     "{\"color\":\"#%02x%02x%02x\",\"color2\":\"%s\",\"dev\":%s,\"name\":\"%s\",\"bright\":%u,"
                     "\"version\":\"%s\",\"addr\":\"%s\",\"admin\":%s,\"admin_key\":%s,\"trusted\":%s,\"peers\":[",
                     c->r, c->g, c->b, c2, c->dev_mode ? "true" : "false", name, c->bright,
                     esp_app_get_description()->version, mesh_own_addr(), c->admin ? "true" : "false", akey, tkey);
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

// "wave centre|left|right|top|bottom": a wave of this board's colour here and on every board nearby, as a tap on main
// does ("mesh: wave <kind> from self"; the main page draws it while it is shown: "display: wave frames ..." after)
static void cmd_wave(int argc, char **argv)
{
    uint8_t k = argc == 2 ? mesh_wave_kind(argv[1]) : WAVE_NONE;
    if (k == WAVE_NONE) { ESP_LOGW(TAG, "error wave: expected centre|left|right|top|bottom"); return; }
    mesh_start_wave(k);
    ESP_LOGI(TAG, "ok wave %s", mesh_wave_name(k));
}

// "game": the round's state; "game start": a round from this board, as a tap on the game page; "game pick
// rock|paper|scissors": the own pick during the countdown ("mesh: game ..." lines, then "mesh: game result ...")
static void cmd_game(int argc, char **argv)
{
    static const char *const phases[] = { "idle", "count", "collect", "result" };
    if (argc == 2 && !strcmp(argv[1], "start")) {
        if (!mesh_game_start()) { ESP_LOGW(TAG, "error game: a round is running"); return; }
        ESP_LOGI(TAG, "ok game start");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "pick")) {
        uint8_t p = rps_pick_from_name(argv[2]);
        if (!p) { ESP_LOGW(TAG, "error game: expected rock|paper|scissors"); return; }
        if (!mesh_game_pick(p)) { ESP_LOGW(TAG, "error game: no countdown running"); return; }
        ESP_LOGI(TAG, "ok game pick %s", rps_pick_name(p));
        return;
    }
    if (argc != 1) { ESP_LOGW(TAG, "error game: expected start | pick rock|paper|scissors"); return; }
    static mesh_game_t g;
    mesh_game_state(&g);
    ESP_LOGI(TAG, "ok game %s left=%lu own=%s result=%s peers=%d", phases[g.phase], (unsigned long)g.ms_left,
             rps_pick_name(g.own), rps_outcome_name(g.outcome), g.n);
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

/* ---------- secure updates ----------
 * "admin on|off|key|new|selftest" (the board that signs its image for boards out of dev mode), "admin" alone: the
 * state. "trust <base64 of the admin's 64-byte public key>" (88 characters; the console line holds 119), "trust none",
 * "trust" alone: the trusted key in full (it is public), for the harness to put it back. The work that needs the
 * ECDSA runs in the fw task (mesh_fw_admin), which answers the "test: ok ..." line. */
static void cmd_admin(int argc, char **argv)
{
    const app_cfg_t *c = config_get();
    const char *op = argc == 2 ? argv[1] : argc == 1 ? "" : "?";
    if (!*op) {
        char k[FWA_SHORT_LEN + 1] = "none", t[FWA_SHORT_LEN + 1] = "none";
        if (c->has_admin_key) fwa_short_id(c->admin_pub, k);
        if (c->has_trusted) fwa_short_id(c->trusted, t);
        ESP_LOGI(TAG, "admin %s key %s trusted %s", c->admin ? "on" : "off", k, t);
    } else if (!strcmp(op, "on")) {
        mesh_fw_admin(MESH_FW_ADMIN_ON);
    } else if (!strcmp(op, "off")) {
        mesh_fw_admin(MESH_FW_ADMIN_OFF);
    } else if (!strcmp(op, "new")) {
        mesh_fw_admin(MESH_FW_ADMIN_NEW);
    } else if (!strcmp(op, "selftest")) {
        mesh_fw_admin(MESH_FW_ADMIN_SELFTEST);
    } else if (!strcmp(op, "key")) {
        if (!c->has_admin_key) { ESP_LOGW(TAG, "error admin key: no key yet (admin on makes one)"); return; }
        char b[FWA_B64_PUB_LEN + 1];
        fwa_b64_encode(c->admin_pub, FWA_PUB_LEN, b, sizeof(b));
        ESP_LOGI(TAG, "admin key %s", b);
    } else {
        ESP_LOGW(TAG, "error admin: expected on|off|key|new|selftest");
    }
}

static void cmd_trust(int argc, char **argv)
{
    const app_cfg_t *c = config_get();
    if (argc == 1) {
        char b[FWA_B64_PUB_LEN + 1] = "none";
        if (c->has_trusted) fwa_b64_encode(c->trusted, FWA_PUB_LEN, b, sizeof(b));
        ESP_LOGI(TAG, "trust %s", b);
        return;
    }
    if (argc != 2) { ESP_LOGW(TAG, "error trust: expected a base64 public key or none"); return; }
    if (!strcmp(argv[1], "none")) { mesh_fw_trust(NULL); return; }
    uint8_t pub[FWA_PUB_LEN + 3];
    if (strlen(argv[1]) != FWA_B64_PUB_LEN || fwa_b64_decode(argv[1], pub, sizeof(pub)) != FWA_PUB_LEN) {
        ESP_LOGW(TAG, "error trust: expected the %d-character base64 key that \"admin key\" prints", FWA_B64_PUB_LEN);
        return;
    }
    if (!mesh_fw_trust(pub)) ESP_LOGW(TAG, "error trust: busy, try again");
}

/* ---------- shot ----------
 * "shot [screen] [div]": a screen (the registry's names; default "shown", the one on the display) sent over USB as
 * base64 RGB565, every div-th pixel of every div-th row (div 1..4, default 2): "SHOT name w h", one "S:" line per
 * row, "SHOTEND". tools/shot.py turns a log into PNGs. From esp32-s3-meteobus (port/esp32-p4, main/console.c).
 * "shot wave [div]": the wave frame shown now (main, or pick / voice / game with the page's picture on top) (the screens' snapshot is LVGL's page, not the raw frames
 * a wave is drawn with), computed by the same wave_fill with the same inputs as ui.c's wave loop.
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
    uint16_t *wave_px = NULL;
    bool got;
    if (!strcmp(name, "wave")) {
        static wave_t ws[WAVE_MAX];
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        int nw = mesh_waves(ws, WAVE_MAX, now);
        wave_px = nw > 0 ? heap_caps_malloc((size_t)DISP_W * DISP_H * 2, MALLOC_CAP_SPIRAM) : NULL;
        if (wave_px) {
            uint8_t hr, hg, hb;
            mesh_get_mix(&hr, &hg, &hb);
            const app_cfg_t *c = config_get();
            bool hold = mesh_holding() || mesh_peer_holding();
            display_lock(-1);                             // wave_fill is not reentrant: the wave loop holds this lock
            for (int y = 0; y < DISP_H; y += 32)
                wave_fill(wave_px + (size_t)y * DISP_W, y, DISP_H - y < 32 ? DISP_H - y : 32, ws, nw, now, hr, hg, hb,
                          hold, c->r, c->g, c->b);
            for (int y = 0; y < DISP_H; y += 32)          // the page on top, as the loop draws it (pick, voice, game)
                ui_wave_layer(wave_px + (size_t)y * DISP_W, y, DISP_H - y < 32 ? DISP_H - y : 32);
            display_unlock();
            for (size_t i = 0; i < (size_t)DISP_W * DISP_H; i++)   // panel order -> RGB565 little-endian
                wave_px[i] = (uint16_t)(wave_px[i] << 8 | wave_px[i] >> 8);
            img = (web_image_t){ .data = (const uint8_t *)wave_px, .w = DISP_W, .h = DISP_H, .stride = DISP_W * 2 };
            ESP_LOGI(TAG, "shot wave: %d wave(s) at %lu ms", nw, (unsigned long)now);
        }
        got = false;                                      // nothing for screens_snapshot_free
    } else {
        got = screens_snapshot(strcmp(name, "shown") ? name : "current", &img);
    }
    if (!img.data) {
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
    if (got) screens_snapshot_free(&img);                 // lv_draw_buf_destroy under the display lock
    free(wave_px);
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
    testcon_register("color2", "color2 RRGGBB|auto", cmd_color2);
    testcon_register("devmode", "devmode on|off", cmd_devmode);
    testcon_register("name", "name TEXT", cmd_name);
    testcon_register("bright", "bright 0-100", cmd_bright);
    testcon_register("peers", "peers", cmd_peers);
    testcon_register("cfg", "cfg", cmd_cfg);
    testcon_register("hold", "hold on|off", cmd_hold);
    testcon_register("shot", "shot [screen] [div]", cmd_shot);
    testcon_register("fwpull", "fwpull ADDR|ORIGIN", cmd_fwpull);
    testcon_register("wave", "wave centre|left|right|top|bottom", cmd_wave);
    testcon_register("game", "game [start | pick rock|paper|scissors]", cmd_game);
    testcon_register("admin", "admin [on|off|key|new|selftest]", cmd_admin);
    testcon_register("trust", "trust [KEY|none]", cmd_trust);
}
