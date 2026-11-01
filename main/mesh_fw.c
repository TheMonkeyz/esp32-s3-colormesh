// Dev-mode firmware pull over Bluetooth (mesh_fw.h; docs/RESEARCH-BLE.md "The firmware link", plan step 5).
//
// Who does what: a dev-mode board that sees a strictly newer servable version picks that peer (highest version,
// then lowest address), advertises connectable with target = that peer's origin and waits. The named peer connects
// as central and streams its running image with Write Without Response; the dev board (peripheral, GATT server)
// checks, writes, ACKs every 4 KB sector, verifies, sets the boot slot and restarts. The new image boots
// PENDING_VERIFY and confirms itself after 60 s with the display up and the host synced.
//
// GATT service (on every board, used only while pulling), 128-bit UUIDs 6c6d000N-8e6b-4c1b-9a35-2d5e7f0c4b31:
//   0001 service · 0002 CTRL (write): START = 01 · len u32 LE · SHA-256[32] · ver[4] (41 bytes), ABORT = 02
//   0003 DATA (write without response): sector u16 LE · seq u8 · data. A sector is packets seq 0, 1, ... then one
//        seq 0xFF packet carrying the rest of the sector and the sector's CRC16-CCITT (poly 0x1021, init 0, LE) in
//        its last two bytes (Espressif's ble_ota framing, without the component). 509 bytes at MTU 512.
//   0004 ACK (notify, 20 bytes): sector u16 LE · status (0 ok, 1 CRC error: resend from it, 2 abort) · zeros.
//        Sector 0xFFFF = the end: status 0 image verified and boot slot set, 2 failed.
// The sender keeps a window of 4 sectors beyond the last ACKed one; the receiver has a PSRAM ring of 8 sectors.
//
// Threads: the GAP hook and the GATT callbacks run in the NimBLE host task and only copy and post; every NimBLE
// procedure, flash write, mesh_* call and log line is in the "fw" task (core 0, 5 KB internal stack: fwloop, which
// runs esp_ota_end, left 732 bytes of 4 KB).
#include "mesh_fw.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "host/ble_hs.h"
#include "host/ble_esp_gap.h"
#include "testcon.h"
#include "board.h"
#include "config.h"
#include "mesh.h"
#include "mesh_proto.h"
#include "ui.h"

static const char *TAG = "fw";

#define SECTOR          4096
#define RING            8           // receiver: sectors buffered in PSRAM
#define WINDOW          4           // sender: sectors sent beyond the last ACKed one
#define PKT_MAX         509         // ATT MTU 512 - 3
#define SEQ_LAST        0xFF
#define ACK_LEN         20
#define ACK_DONE        0xFFFF
#define ST_OK           0
#define ST_CRC          1
#define ST_ABORT        2
#define CTRL_START      0x01
#define CTRL_ABORT      0x02
#define START_LEN       41
#define BACKOFF_MS      30000       // after a failed pull
#define WANT_MS         60000       // waiting for the target to connect
#define LINK_IDLE_MS    15000       // no packet / no ACK for this long: give up
#define CONNECT_MS      10000
#define LOG_STEP        (128 * 1024)
#define POLL_MS         1000
#define CONFIRM_MS      60000

// MESH_PEER_HAS_TARGET: mesh.h's mesh_peer_t carries the peer's advertised target origin (see the report: agent A4
// adds `uint8_t target;` + `o->target = p->st.target;` in mesh_peers()). Without it a sender serves a dev peer older
// than itself when it is the best server that peer can see (the receiver's own rule), and the receiver refuses a
// central that is not its target.

#define CM_UUID(n) BLE_UUID128_INIT(0x31, 0x4b, 0x0c, 0x7f, 0x5e, 0x2d, 0x35, 0x9a, \
                                    0x1b, 0x4c, 0x6b, 0x8e, (n), 0x00, 0x6d, 0x6c)
static const ble_uuid128_t svc_uuid = CM_UUID(0x01);
static const ble_uuid128_t ctrl_uuid = CM_UUID(0x02);
static const ble_uuid128_t data_uuid = CM_UUID(0x03);
static const ble_uuid128_t ack_uuid = CM_UUID(0x04);
static uint16_t ack_handle;

// ---- events to the fw task: type << 24 | argument ----
enum { E_TARGET = 1, E_PCONN, E_PDISC, E_START, E_SECTOR, E_NACK, E_RXERR, E_INFO, E_LOOP, E_CONFIRM };
#define EV(t, a) (((uint32_t)(t) << 24) | ((uint32_t)(a) & 0xFFFFFF))
enum { RXE_ABORT = 1, RXE_NOT_NEWER, RXE_BUSY };
static const char *const rxe_text[] = { "", "the sender aborted", "offered version is not newer",
                                        "ring slot still busy (sender ignored the window)" };

// sender link bits (host task -> fw task)
#define B_CONN      BIT0
#define B_CONN_FAIL BIT1
#define B_DISC      BIT2
#define B_MTU       BIT3
#define B_PHY       BIT4
#define B_DLE       BIT5
#define B_GATT      BIT6
#define B_ACK       BIT7
#define B_ALL       0xFF

static QueueHandle_t evq;           // GAP hook, CTRL, console, timer -> fw task
static QueueHandle_t secq;          // DATA (sector full / CRC error / protocol error) -> fw task
static EventGroupHandle_t eg;
static TaskHandle_t fw_task_h;
static esp_timer_handle_t confirm_timer;

// ---- this board's image ----
static const esp_partition_t *run_part;
static const char *run_state_name = "none";
static volatile bool img_servable, img_pending;
static uint32_t run_len;
static uint8_t run_digest[32];
static bool run_hashed;
static uint8_t own_ver[4];
static char bad_version[32];        // the version of the last image that rolled back ("" none)
static uint8_t bad_ver[4];

static volatile int progress = -1;
static volatile uint8_t forced = MP_TARGET_NONE;

// ---- receiver ----
typedef enum { R_IDLE, R_WANT, R_LINKED, R_LOOP } rstate_t;
static struct {
    volatile rstate_t state;
    volatile uint16_t conn;
    uint8_t target, peer_addr[6];   // fw task
    bool forced;                    // this pull skips the version checks (fwpull)
    bool loop;                      // fwloop: packets come from this board's own sender code, no Bluetooth
    uint32_t want_ms;
    // START (CTRL callback writes before setting active)
    uint32_t len, nsec;
    uint8_t ver[4];
    volatile bool active;
    // packet assembly: host task (or the fw task in fwloop)
    uint32_t cur, off;
    uint8_t seq;
    bool resync;
    volatile uint32_t last_ms, nacks;
    volatile bool slot_full[RING];
    // writing: fw task
    uint32_t next_write, logged;
    esp_ota_handle_t h;
    bool begun;
    const esp_partition_t *part;
    int64_t t0_us;
    uint32_t write_us_max;
    char version[32];
    bool done_ok;
} rx = { .conn = BLE_HS_CONN_HANDLE_NONE };
static uint8_t *ring;               // RING * SECTOR in PSRAM, allocated on the first pull, kept
static uint8_t rxbuf[PKT_MAX + 4];  // host task only
static uint8_t pconn_addr[6];       // the central of the last peripheral connection (host task -> fw task)
static uint32_t backoff_until;
static bool noted_missing, noted_devmode;

// ---- sender ----
static struct {
    volatile uint16_t conn;
    volatile int conn_status, disc_reason, gatt_status;
    volatile uint16_t mtu, dle;
    volatile uint8_t phy;
    uint16_t svc_start, svc_end, h_ctrl, h_data, h_ack;
    volatile int32_t acked, nack;
    volatile bool abort_rx, loop;
    volatile int done;
    uint32_t enomem, resent;
} tx = { .conn = BLE_HS_CONN_HANDLE_NONE };
static uint8_t txbuf[PKT_MAX];
static uint8_t cool_addr[6];
static uint32_t cool_until, next_serve_ms;
static bool loop_corrupt;

static mesh_peer_t peers[MP_MAX_PEERS];   // fw task

// ---- helpers ----

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void post(QueueHandle_t q, uint32_t e) { if (q) xQueueSend(q, &e, 0); }

static void fmt_addr(char *out, size_t n, const uint8_t a[6])
{
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static bool addr_lt(const uint8_t a[6], const uint8_t b[6])
{
    for (int i = 5; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i];
    return false;
}

static uint16_t crc16(const uint8_t *p, size_t n)     // CRC16-CCITT, poly 0x1021, init 0 (XMODEM)
{
    uint16_t c = 0;
    while (n--) {
        c ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static uint32_t sector_len(uint32_t len, uint32_t idx)
{
    uint32_t o = idx * SECTOR;
    return len - o < SECTOR ? len - o : SECTOR;
}

// "12.3" from a value in tenths
static void tenths(char *out, size_t n, uint64_t v10) { snprintf(out, n, "%lu.%lu", (unsigned long)(v10 / 10), (unsigned long)(v10 % 10)); }

static void fmt_rate(char *s, size_t sn, char *r, size_t rn, uint32_t bytes, int64_t us)
{
    if (us < 1) us = 1;
    tenths(s, sn, (uint64_t)us / 100000);
    tenths(r, rn, (uint64_t)bytes * 10000000ULL / 1024 / (uint64_t)us);   // KB/s in tenths
}

static bool ensure_ring(void)
{
    if (!ring) ring = heap_caps_malloc(RING * SECTOR, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ring != NULL;
}

// ======================================================================================================
// Receiver: packets (host task, or the fw task in fwloop)
// ======================================================================================================

static void rx_packet(const uint8_t *p, int len)
{
    if (!rx.active || len < 3 || !ring) return;
    rx.last_ms = now_ms();
    uint32_t idx = p[0] | (uint32_t)p[1] << 8;
    uint8_t seq = p[2];
    if (idx != rx.cur || idx >= rx.nsec) return;      // stale (sent before our CRC error) or past the end
    if (rx.resync) {
        if (seq != 0) return;                         // wait for the resend, which starts at seq 0
        rx.resync = false;
        rx.off = 0;
        rx.seq = 0;
    }
    uint32_t n = sector_len(rx.len, idx);
    uint8_t *s = ring + (idx % RING) * SECTOR;
    if (rx.off == 0 && seq == 0 && rx.slot_full[idx % RING]) {
        rx.active = false;
        post(secq, EV(E_RXERR, RXE_BUSY));
        return;
    }
    int dlen = len - 3 - (seq == SEQ_LAST ? 2 : 0);
    bool bad = dlen < 0 || rx.off + (uint32_t)dlen > n || (seq != SEQ_LAST && seq != rx.seq);
    if (!bad) {
        memcpy(s + rx.off, p + 3, dlen);
        rx.off += dlen;
        if (seq != SEQ_LAST) { rx.seq++; return; }
        uint16_t c = p[len - 2] | (uint16_t)p[len - 1] << 8;
        bad = rx.off != n || crc16(s, n) != c;
    }
    if (bad) {                                        // ask for the sector again (go-back-N)
        rx.resync = true;
        rx.off = 0;
        rx.seq = 0;
        rx.nacks++;
        post(secq, EV(E_NACK, idx));
        return;
    }
    rx.slot_full[idx % RING] = true;
    rx.cur++;
    rx.off = 0;
    rx.seq = 0;
    post(secq, EV(E_SECTOR, idx));
}

// ---- GATT server (host task) ----

static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t b[START_LEN];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), &n) != 0 || n < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (rx.state != R_LINKED || conn != rx.conn) return BLE_ATT_ERR_UNLIKELY;
    if (b[0] == CTRL_ABORT) {
        rx.active = false;
        post(evq, EV(E_RXERR, RXE_ABORT));
        return 0;
    }
    if (b[0] != CTRL_START || n != START_LEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint32_t len = b[1] | (uint32_t)b[2] << 8 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 24;
    if (rx.active || !ring || !rx.part || len < SECTOR || len > rx.part->size) return BLE_ATT_ERR_UNLIKELY;
    if (!rx.forced && mp_ver_cmp(b + 37, own_ver) <= 0) {
        post(evq, EV(E_RXERR, RXE_NOT_NEWER));
        return BLE_ATT_ERR_UNLIKELY;
    }
    rx.len = len;
    rx.nsec = (len + SECTOR - 1) / SECTOR;
    memcpy(rx.ver, b + 37, 4);
    rx.cur = rx.off = 0;
    rx.seq = 0;
    rx.resync = false;
    rx.nacks = 0;
    for (int i = 0; i < RING; i++) rx.slot_full[i] = false;
    rx.last_ms = now_ms();
    rx.active = true;
    post(evq, EV(E_START, 0));
    return 0;
}

static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (conn != rx.conn || !rx.active) return 0;
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, rxbuf, sizeof(rxbuf), &n) != 0) return 0;
    rx_packet(rxbuf, n);
    return 0;
}

static int ack_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_UNLIKELY;                      // notify only
}

static const struct ble_gatt_svc_def svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &ctrl_uuid.u, .access_cb = ctrl_access, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &data_uuid.u, .access_cb = data_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &ack_uuid.u, .access_cb = ack_access, .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &ack_handle },
            { 0 },
        },
    },
    { 0 },
};

// GAP events of the mesh's own advertising (the peripheral side of a pull); host task
static void gap_hook(struct ble_gap_event *e)
{
    if (e->type == BLE_GAP_EVENT_CONNECT && e->connect.status == 0) {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(e->connect.conn_handle, &d) == 0 && d.role == BLE_GAP_ROLE_SLAVE) {
            memcpy(pconn_addr, d.peer_id_addr.val, 6);
            post(evq, EV(E_PCONN, e->connect.conn_handle));
        }
    } else if (e->type == BLE_GAP_EVENT_DISCONNECT) {
        if (e->disconnect.conn.conn_handle == rx.conn) rx.active = false;
        post(evq, EV(E_PDISC, e->disconnect.conn.conn_handle));
    }
}

// ======================================================================================================
// Receiver: fw task
// ======================================================================================================

static void tx_on_ack(uint32_t idx, uint8_t st);

static int send_ack(uint16_t idx, uint8_t st)
{
    if (rx.loop) { tx_on_ack(idx, st); return 0; }
    uint8_t b[ACK_LEN] = { (uint8_t)idx, (uint8_t)(idx >> 8), st };
    for (int i = 0; i < 200; i++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(b, sizeof(b));
        int rc = om ? ble_gatts_notify_custom(rx.conn, ack_handle, om) : BLE_HS_ENOMEM;   // frees om
        if (rc != BLE_HS_ENOMEM) return rc;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return BLE_HS_ENOMEM;
}

// Ends a pull (any state): radio back to normal; with backoff, the next automatic pull waits 30 s
static void rx_stop(bool backoff)
{
    rx.active = false;
    if (rx.begun) { esp_ota_abort(rx.h); rx.begun = false; }
    bool linked = rx.state == R_LINKED;
    if (!rx.loop && rx.state != R_IDLE) {
        if (linked && rx.conn != BLE_HS_CONN_HANDLE_NONE) {
            send_ack(ACK_DONE, ST_ABORT);
            ble_gap_terminate(rx.conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        mesh_set_target(MP_TARGET_NONE);
        mesh_set_connectable(false);
        mesh_adv_pause(false);
    }
    if (rx.loop) send_ack(ACK_DONE, ST_ABORT);        // fwloop: the sender side stops at once
    rx.conn = BLE_HS_CONN_HANDLE_NONE;
    rx.state = R_IDLE;
    rx.loop = false;
    progress = -1;
    if (backoff) backoff_until = now_ms() + BACKOFF_MS;
}

static void rx_fail(const char *fmt, ...)
{
    char why[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "failed: %s", why);
    rx_stop(true);
}

// The first sector: an app image of this project, newer than ours and not the one that rolled back
static const char *rx_check_header(const uint8_t *s, uint32_t n)
{
    const size_t at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);   // 32
    if (n < at + sizeof(esp_app_desc_t)) return "image too short";
    if (s[0] != ESP_IMAGE_HEADER_MAGIC) return "not an app image";
    esp_app_desc_t d;
    memcpy(&d, s + at, sizeof(d));
    if (d.magic_word != ESP_APP_DESC_MAGIC_WORD) return "no app description";
    if (strncmp(d.project_name, esp_app_get_description()->project_name, sizeof(d.project_name)))
        return "another project";
    memcpy(rx.version, d.version, sizeof(rx.version));
    rx.version[sizeof(rx.version) - 1] = 0;
    if (rx.forced) return NULL;
    uint8_t v[4];
    mp_ver_from_string(rx.version, v);
    if (mp_ver_cmp(v, own_ver) <= 0) return "image version is not newer";
    if (bad_version[0] && !strncmp(rx.version, bad_version, sizeof(bad_version))) return "that version rolled back here";
    return NULL;
}

static void rx_finish(void)
{
    esp_err_t err = esp_ota_end(rx.h);                // magic, segments, checksum, appended SHA-256
    rx.begun = false;
    if (err != ESP_OK) { rx_fail("verify: %s", esp_err_to_name(err)); return; }
    char a[18], s[12], r[12];
    fmt_addr(a, sizeof(a), rx.peer_addr);
    fmt_rate(s, sizeof(s), r, sizeof(r), rx.len, esp_timer_get_time() - rx.t0_us);
    if (rx.loop) {                                    // fwloop: verified copy, boot slot unchanged
        rx.done_ok = true;
        send_ack(ACK_DONE, ST_OK);
        rx.state = R_IDLE;
        rx.loop = false;
        progress = -1;
        return;
    }
    err = esp_ota_set_boot_partition(rx.part);
    if (err != ESP_OK) { rx_fail("set boot: %s", esp_err_to_name(err)); return; }
    progress = 100;
    send_ack(ACK_DONE, ST_OK);
    ESP_LOGI(TAG, "pulled %s from %s in %s s (%s KB/s, %lu resent), restarting", rx.version, a, s, r,
             (unsigned long)rx.nacks);
    vTaskDelay(pdMS_TO_TICKS(300));                   // let the final ACK go out
    ble_gap_terminate(rx.conn, BLE_ERR_REM_USER_CONN_TERM);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void rx_write_sector(uint32_t idx)
{
    if (rx.state != R_LINKED && rx.state != R_LOOP) return;
    if (idx != rx.next_write) { rx_fail("sector %lu out of order", (unsigned long)idx); return; }
    uint8_t *s = ring + (idx % RING) * SECTOR;
    uint32_t n = sector_len(rx.len, idx);
    if (idx == 0) {
        const char *why = rx_check_header(s, n);
        if (why) { rx_fail("%s", why); return; }
        esp_err_t err = esp_ota_begin(rx.part, OTA_WITH_SEQUENTIAL_WRITES, &rx.h);
        if (err != ESP_OK) { rx_fail("ota_begin: %s", esp_err_to_name(err)); return; }
        rx.begun = true;
    }
    int64_t t = esp_timer_get_time();
    esp_err_t err = esp_ota_write(rx.h, s, n);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t);
    if (us > rx.write_us_max) rx.write_us_max = us;
    rx.slot_full[idx % RING] = false;
    if (err != ESP_OK) { rx_fail("ota_write: %s", esp_err_to_name(err)); return; }
    rx.next_write++;
    send_ack(idx, ST_OK);
    uint32_t done = idx * SECTOR + n;
    progress = (int)((uint64_t)done * 99 / rx.len);
    if (!rx.loop && (done / LOG_STEP != rx.logged || rx.next_write == rx.nsec)) {
        rx.logged = done / LOG_STEP;
        ESP_LOGI(TAG, "%lu/%lu KB", (unsigned long)(done / 1024), (unsigned long)(rx.len / 1024));
    }
    if (rx.next_write == rx.nsec) rx_finish();
}

static void rx_on_sector_event(uint32_t e)
{
    uint32_t a = e & 0xFFFFFF;
    switch (e >> 24) {
    case E_SECTOR: rx_write_sector(a); break;
    case E_NACK: send_ack((uint16_t)a, ST_CRC); break;
    case E_RXERR: if (rx.state != R_IDLE) rx_fail("%s", rxe_text[a < 4 ? a : 0]); break;
    }
}

// Every second: choose a peer to pull from (dev mode), watch the timeouts
static void rx_poll(int n)
{
    uint32_t now = now_ms();
    bool dev = config_get()->dev_mode;
    if (rx.state == R_WANT) {
        if (!dev) { ESP_LOGI(TAG, "pull stopped: dev mode off"); rx_stop(false); return; }
        if (now - rx.want_ms > WANT_MS) {
            char a[18];
            fmt_addr(a, sizeof(a), rx.peer_addr);
            rx_fail("no connection from %s in %d s", a, WANT_MS / 1000);
        }
        return;
    }
    if (rx.state == R_LINKED) {
        if (now - rx.last_ms > LINK_IDLE_MS) rx_fail("no data for %d s", LINK_IDLE_MS / 1000);
        return;
    }
    if (rx.state != R_IDLE) return;
    uint8_t f = forced;
    if (!dev) {
        if (f != MP_TARGET_NONE && !noted_devmode) { ESP_LOGI(TAG, "fwpull %02x waits for dev mode", f); noted_devmode = true; }
        return;
    }
    if (img_pending || !mesh_synced() || (int32_t)(now - backoff_until) < 0) return;
    const mesh_peer_t *best = NULL;
    if (f != MP_TARGET_NONE) {
        for (int i = 0; i < n; i++)
            if (peers[i].origin == f) best = &peers[i];
        if (!best) {
            if (!noted_missing) { ESP_LOGI(TAG, "want %02x: no such peer (waiting)", f); noted_missing = true; }
            return;
        }
    } else {
        for (int i = 0; i < n; i++) {
            const mesh_peer_t *p = &peers[i];
            if (!p->servable || mp_ver_cmp(p->ver, own_ver) <= 0) continue;
            if (bad_version[0] && !memcmp(p->ver, bad_ver, 4)) continue;
            int c = best ? mp_ver_cmp(p->ver, best->ver) : 1;
            if (c > 0 || (c == 0 && addr_lt(p->addr, best->addr))) best = p;
        }
        if (!best) return;
    }
    char a[18], v[24];
    fmt_addr(a, sizeof(a), best->addr);
    ui_fmt_ver(best->ver, v, sizeof(v));
    rx.part = esp_ota_get_next_update_partition(NULL);
    if (!ensure_ring() || !rx.part) { rx_fail("%s", rx.part ? "no PSRAM for the ring" : "no update slot"); return; }
    rx.target = best->origin;
    memcpy(rx.peer_addr, best->addr, 6);
    rx.forced = f != MP_TARGET_NONE;
    rx.loop = false;
    rx.want_ms = now;
    rx.state = R_WANT;
    mesh_set_target(best->origin);
    mesh_set_connectable(true);
    ESP_LOGI(TAG, "want %s from %s%s", v, a, rx.forced ? " (fwpull)" : "");
}

static void rx_on_connect(uint16_t conn)
{
    char a[18];
    fmt_addr(a, sizeof(a), pconn_addr);
    if (rx.state != R_WANT || pconn_addr[0] != rx.target) {
        ESP_LOGW(TAG, "refused %s: %s", a, rx.state != R_WANT ? "not pulling" : "not the target");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    mesh_adv_pause(true);                             // first: the mesh task would restart the connectable advert
    mesh_set_connectable(false);
    memcpy(rx.peer_addr, pconn_addr, 6);
    rx.conn = conn;
    rx.active = false;
    rx.next_write = 0;
    rx.logged = 0;
    rx.write_us_max = 0;
    rx.last_ms = now_ms();
    rx.state = R_LINKED;
    progress = 0;
    ESP_LOGI(TAG, "linked to %s", a);
}

// ======================================================================================================
// Sender
// ======================================================================================================

static void tx_on_ack(uint32_t idx, uint8_t st)
{
    if (idx == ACK_DONE) { tx.done = st; if (st != ST_OK) tx.abort_rx = true; }
    else if (st == ST_OK) { if ((int32_t)idx > tx.acked) tx.acked = (int32_t)idx; }
    else if (st == ST_CRC) tx.nack = (int32_t)idx;
    else tx.abort_rx = true;
    if (eg) xEventGroupSetBits(eg, B_ACK);
}

static int tx_gap_cb(struct ble_gap_event *e, void *arg)      // host task
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) { tx.conn = e->connect.conn_handle; xEventGroupSetBits(eg, B_CONN); }
        else { tx.conn_status = e->connect.status; xEventGroupSetBits(eg, B_CONN_FAIL); }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        tx.disc_reason = e->disconnect.reason;
        xEventGroupSetBits(eg, B_DISC);
        break;
    case BLE_GAP_EVENT_MTU:
        tx.mtu = e->mtu.value;
        xEventGroupSetBits(eg, B_MTU);
        break;
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        if (e->phy_updated.status == 0) tx.phy = e->phy_updated.tx_phy;
        xEventGroupSetBits(eg, B_PHY);
        break;
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        tx.dle = e->data_len_chg.max_tx_octets;
        xEventGroupSetBits(eg, B_DLE);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (e->notify_rx.attr_handle == tx.h_ack && OS_MBUF_PKTLEN(e->notify_rx.om) >= 3) {
            uint8_t b[3];
            os_mbuf_copydata(e->notify_rx.om, 0, 3, b);
            tx_on_ack(b[0] | (uint32_t)b[1] << 8, b[2]);
        }
        break;
    default:
        break;
    }
    return 0;
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (err->status == 0) tx.mtu = mtu;
    xEventGroupSetBits(eg, B_MTU);
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) { tx.svc_start = svc->start_handle; tx.svc_end = svc->end_handle; return 0; }
    tx.gatt_status = err->status == BLE_HS_EDONE ? (tx.svc_end ? 0 : BLE_HS_ENOENT) : err->status;
    xEventGroupSetBits(eg, B_GATT);
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        if (!ble_uuid_cmp(&chr->uuid.u, &ctrl_uuid.u)) tx.h_ctrl = chr->val_handle;
        else if (!ble_uuid_cmp(&chr->uuid.u, &data_uuid.u)) tx.h_data = chr->val_handle;
        else if (!ble_uuid_cmp(&chr->uuid.u, &ack_uuid.u)) tx.h_ack = chr->val_handle;
        return 0;
    }
    tx.gatt_status = err->status == BLE_HS_EDONE ? ((tx.h_ctrl && tx.h_data && tx.h_ack) ? 0 : BLE_HS_ENOENT)
                                                 : err->status;
    xEventGroupSetBits(eg, B_GATT);
    return 0;
}

static int write_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    tx.gatt_status = err->status;
    xEventGroupSetBits(eg, B_GATT);
    return 0;
}

typedef int (*emit_fn)(const uint8_t *pkt, int len);

// One sector as packets: seq 0, 1, ... with up to dmax data bytes, then seq 0xFF with the rest and the CRC
static int tx_sector(const uint8_t *img, uint32_t len, uint32_t idx, int dmax, emit_fn emit)
{
    const uint8_t *s = img + idx * SECTOR;
    uint32_t n = sector_len(len, idx), off = 0;
    uint16_t crc = crc16(s, n);
    uint8_t seq = 0;
    txbuf[0] = (uint8_t)idx;
    txbuf[1] = (uint8_t)(idx >> 8);
    do {
        uint32_t k = n - off < (uint32_t)dmax ? n - off : (uint32_t)dmax;
        txbuf[2] = seq++;
        memcpy(txbuf + 3, s + off, k);
        off += k;
        int rc = emit(txbuf, 3 + k);
        if (rc) return rc;
    } while (n - off > (uint32_t)(dmax - 2));
    uint32_t k = n - off;
    txbuf[2] = SEQ_LAST;
    memcpy(txbuf + 3, s + off, k);
    txbuf[3 + k] = (uint8_t)crc;
    txbuf[4 + k] = (uint8_t)(crc >> 8);
    return emit(txbuf, 5 + k);
}

static int emit_ble(const uint8_t *p, int len)
{
    uint32_t t0 = now_ms();
    for (;;) {
        if (xEventGroupGetBits(eg) & B_DISC) return BLE_HS_ENOTCONN;
        int rc = ble_gattc_write_no_rsp_flat(tx.conn, tx.h_data, p, len);
        if (rc != BLE_HS_ENOMEM) return rc;
        tx.enomem++;                                  // mbuf pool empty: back off one tick, never a loss
        if (now_ms() - t0 > 10000) return rc;
        vTaskDelay(1);
    }
}

static void loop_drain(void)
{
    uint32_t e;
    while (xQueueReceive(secq, &e, 0) == pdTRUE) rx_on_sector_event(e);
}

static int emit_loop(const uint8_t *p, int len)
{
    if (loop_corrupt && (p[0] | p[1] << 8) == 3 && p[2] == 2) {   // once: a bad byte in sector 3 -> CRC error
        loop_corrupt = false;
        txbuf[10] ^= 0x55;
    }
    rx_packet(p, len);
    return 0;
}

// Streams the image; NULL when every sector is ACKed, else why it stopped
static const char *tx_stream(const uint8_t *img, uint32_t len, int dmax, emit_fn emit)
{
    uint32_t nsec = (len + SECTOR - 1) / SECTOR, next = 0, logged = 0, last_ms = now_ms();
    int32_t last_acked = -1;
    tx.acked = -1;
    tx.nack = -1;
    tx.abort_rx = false;
    tx.done = -1;
    tx.enomem = tx.resent = 0;
    for (;;) {
        xEventGroupClearBits(eg, B_ACK);
        if (!tx.loop && (xEventGroupGetBits(eg) & B_DISC)) return "link lost";
        if (tx.abort_rx) return "the receiver aborted";
        int32_t nk = tx.nack;
        if (nk >= 0) { tx.nack = -1; next = (uint32_t)nk; tx.resent++; }
        int32_t a = tx.acked;
        if (a != last_acked) {
            last_acked = a;
            last_ms = now_ms();
            uint32_t done = a + 1 >= (int32_t)nsec ? len : (uint32_t)(a + 1) * SECTOR;
            if (!tx.loop && (done / LOG_STEP != logged || done == len)) {
                logged = done / LOG_STEP;
                ESP_LOGI(TAG, "sent %lu/%lu KB", (unsigned long)(done / 1024), (unsigned long)(len / 1024));
            }
        }
        if (a >= (int32_t)nsec - 1) return NULL;
        if (next < nsec && (int32_t)next <= a + WINDOW) {
            int rc = tx_sector(img, len, next, dmax, emit);
            if (rc) return rc == BLE_HS_ENOTCONN ? "link lost" : "write failed";
            next++;
            if (tx.loop) loop_drain();
            continue;
        }
        if (now_ms() - last_ms > LINK_IDLE_MS) return "no ACK for 15 s";
        if (tx.loop) { loop_drain(); if (tx.acked == last_acked && !tx.abort_rx && tx.nack < 0) return "fwloop stalled"; continue; }
        xEventGroupWaitBits(eg, B_ACK | B_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
    }
}

static EventBits_t wait_bits(EventBits_t want, bool all, int ms)
{
    return xEventGroupWaitBits(eg, want | B_DISC, pdFALSE, all ? pdTRUE : pdFALSE, pdMS_TO_TICKS(ms));
}

// Connected as central: link settings, discovery, START, the stream, the final ACK. NULL on success.
static const char *tx_session(const uint8_t *img)
{
    uint16_t c = tx.conn;
    int rc = ble_hs_hci_util_set_data_len(c, 251, 2120);
    if (rc) ESP_LOGW(TAG, "set_data_len rc=%d", rc);
    ble_att_set_preferred_mtu(512);
    rc = ble_gattc_exchange_mtu(c, mtu_cb, NULL);
    if (rc) ESP_LOGW(TAG, "exchange_mtu rc=%d", rc);
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(c, &d) == 0 && d.conn_itvl != 6) {
        struct ble_gap_upd_params up = { .itvl_min = 6, .itvl_max = 6, .latency = 0, .supervision_timeout = 600,
                                         .min_ce_len = 12, .max_ce_len = 24 };
        rc = ble_gap_update_params(c, &up);
        if (rc) ESP_LOGW(TAG, "update_params rc=%d", rc);
    }
    rc = ble_gap_set_prefered_le_phy(c, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
    if (rc) ESP_LOGW(TAG, "set_prefered_le_phy rc=%d", rc);
    EventBits_t b = wait_bits(B_MTU | B_PHY | B_DLE, true, 2000);
    if (b & B_DISC) return "link lost";
    uint8_t txp = 0, rxp = 0;
    ble_gap_read_le_phy(c, &txp, &rxp);
    uint16_t mtu = ble_att_mtu(c);
    int itvl = ble_gap_conn_find(c, &d) == 0 ? d.conn_itvl : 0;
    ESP_LOGI(TAG, "link mtu=%u phy=%s dle=%u itvl=%d.%02d ms%s", mtu, txp == 2 ? "2M" : txp == 1 ? "1M" : "?",
             tx.dle, itvl * 125 / 100, itvl * 125 % 100, (b & (B_MTU | B_PHY | B_DLE)) == (B_MTU | B_PHY | B_DLE) ? "" : " (timeout)");
    if (mtu < 64) return "MTU too small";

    tx.svc_start = tx.svc_end = tx.h_ctrl = tx.h_data = tx.h_ack = 0;
    xEventGroupClearBits(eg, B_GATT);
    if (ble_gattc_disc_svc_by_uuid(c, &svc_uuid.u, svc_cb, NULL)) return "service discovery";
    if (!(wait_bits(B_GATT, false, 5000) & B_GATT) || tx.gatt_status) return "no firmware service";
    xEventGroupClearBits(eg, B_GATT);
    if (ble_gattc_disc_all_chrs(c, tx.svc_start, tx.svc_end, chr_cb, NULL)) return "characteristic discovery";
    if (!(wait_bits(B_GATT, false, 5000) & B_GATT) || tx.gatt_status) return "characteristics missing";

    uint8_t st[START_LEN];
    st[0] = CTRL_START;
    for (int i = 0; i < 4; i++) st[1 + i] = (uint8_t)(run_len >> (8 * i));
    memcpy(st + 5, run_digest, 32);
    memcpy(st + 37, own_ver, 4);
    xEventGroupClearBits(eg, B_GATT);
    if (ble_gattc_write_flat(c, tx.h_ctrl, st, sizeof(st), write_cb, NULL)) return "START write";
    if (!(wait_bits(B_GATT, false, 5000) & B_GATT)) return "no answer to START";
    if (tx.gatt_status) {
        static char why[40];
        snprintf(why, sizeof(why), "START refused (status %d)", tx.gatt_status);
        return why;
    }
    int payload = mtu - 3 < PKT_MAX ? mtu - 3 : PKT_MAX;
    const char *why = tx_stream(img, run_len, payload - 3, emit_ble);
    if (why) return why;
    uint32_t t0 = now_ms();
    while (tx.done < 0 && now_ms() - t0 < 15000) {    // the receiver verifies (esp_ota_end) and answers
        xEventGroupClearBits(eg, B_ACK);
        if (tx.done >= 0) break;
        if (wait_bits(B_ACK, false, 500) & B_DISC) break;
    }
    if (tx.done == ST_OK) return NULL;
    return tx.done < 0 ? "no final ACK" : "the receiver rejected the image";
}

static void serve(const mesh_peer_t *p)
{
    char a[18];
    fmt_addr(a, sizeof(a), p->addr);
    memcpy(cool_addr, p->addr, 6);
    cool_until = now_ms() + BACKOFF_MS;
    next_serve_ms = now_ms() + 3000;
    const void *img = NULL;
    esp_partition_mmap_handle_t mh;
    esp_err_t err = esp_partition_mmap(run_part, 0, run_len, ESP_PARTITION_MMAP_DATA, &img, &mh);
    if (err != ESP_OK) { ESP_LOGW(TAG, "failed: mmap: %s", esp_err_to_name(err)); return; }
    xEventGroupClearBits(eg, B_ALL);
    tx.conn = BLE_HS_CONN_HANDLE_NONE;
    tx.loop = false;
    tx.mtu = tx.dle = 0;
    tx.phy = 0;
    ble_addr_t peer = { .type = BLE_ADDR_PUBLIC };   // every board advertises its public address
    memcpy(peer.val, p->addr, 6);
    struct ble_gap_conn_params cp = { .scan_itvl = 0x30, .scan_window = 0x30, .itvl_min = 6, .itvl_max = 6,
                                      .latency = 0, .supervision_timeout = 600, .min_ce_len = 12, .max_ce_len = 24 };
#ifdef MESH_PEER_HAS_TARGET
    int connect_ms = CONNECT_MS;
#else
    int connect_ms = 3000;                            // a guess without the peer's target: give up sooner
#endif
    mesh_scan_pause(true);                            // ble_gap_connect is refused while scanning
    int rc = ble_gap_connect(mesh_own_addr_type(), &peer, connect_ms, &cp, tx_gap_cb, NULL);
    if (rc) {
        mesh_scan_pause(false);
        ESP_LOGW(TAG, "failed: connect %s rc=%d", a, rc);
        esp_partition_munmap(mh);
        return;
    }
    EventBits_t b = xEventGroupWaitBits(eg, B_CONN | B_CONN_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(connect_ms + 2000));
    mesh_scan_pause(false);
    if (!(b & B_CONN)) {
        if (!(b & B_CONN_FAIL)) ble_gap_conn_cancel();
        ESP_LOGW(TAG, "failed: no connection to %s (status %d)", a, b & B_CONN_FAIL ? tx.conn_status : -1);
        esp_partition_munmap(mh);
        return;
    }
    ESP_LOGI(TAG, "serving %s %s %lu bytes", a, esp_app_get_description()->version, (unsigned long)run_len);
    int64_t t0 = esp_timer_get_time();
    const char *why = tx_session(img);
    int64_t us = esp_timer_get_time() - t0;
    if (!why) {
        char s[12], r[12];
        fmt_rate(s, sizeof(s), r, sizeof(r), run_len, us);
        ESP_LOGI(TAG, "served %s in %s s (%s KB/s, %lu resent, %lu ENOMEM)", a, s, r, (unsigned long)tx.resent,
                 (unsigned long)tx.enomem);
    } else {
        ESP_LOGW(TAG, "failed: %s (serving %s)", why, a);
    }
    if (!(xEventGroupGetBits(eg) & B_DISC)) {
        ble_gap_terminate(tx.conn, BLE_ERR_REM_USER_CONN_TERM);
        xEventGroupWaitBits(eg, B_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
    }
    tx.conn = BLE_HS_CONN_HANDLE_NONE;
    esp_partition_munmap(mh);
}

#ifndef MESH_PEER_HAS_TARGET
// Without the peer's target: would this dev peer pick us? (the receiver's rule: highest version, lowest address)
static bool best_server_for(const mesh_peer_t *dev, int n, const uint8_t own[6])
{
    if (mp_ver_cmp(dev->ver, own_ver) >= 0) return false;
    for (int i = 0; i < n; i++) {
        const mesh_peer_t *q = &peers[i];
        if (q == dev || !q->servable) continue;
        int c = mp_ver_cmp(q->ver, own_ver);
        if (c > 0 || (c == 0 && addr_lt(q->addr, own))) return false;
    }
    return true;
}
#endif

// Every second: a dev peer that wants our image?
static void tx_poll(int n)
{
    if (!img_servable || !run_len || rx.state != R_IDLE || !mesh_synced()) return;
    uint32_t now = now_ms();
    if ((int32_t)(now - next_serve_ms) < 0) return;
    uint8_t me = mesh_own_origin();
#ifndef MESH_PEER_HAS_TARGET
    uint8_t own[6] = { 0 };
    unsigned v[6];
    if (sscanf(mesh_own_addr(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return;
    for (int i = 0; i < 6; i++) own[5 - i] = (uint8_t)v[i];
#endif
    for (int i = 0; i < n; i++) {
        const mesh_peer_t *p = &peers[i];
        if (!p->dev) continue;
#ifdef MESH_PEER_HAS_TARGET
        if (p->target != me) continue;                // the receiver decides (version checks included)
#else
        (void)me;
        if (!best_server_for(p, n, own)) continue;
#endif
        if (!memcmp(p->addr, cool_addr, 6) && (int32_t)(now - cool_until) < 0) continue;
        serve(p);
        return;
    }
}

// ======================================================================================================
// fwinfo, fwloop, confirmation
// ======================================================================================================

static void do_info(void)
{
    const char *read = "nohash";
    uint32_t ms = 0;
    if (run_len && run_hashed) {
        const void *img;
        esp_partition_mmap_handle_t mh;
        int64_t t0 = esp_timer_get_time();
        if (esp_partition_mmap(run_part, 0, run_len, ESP_PARTITION_MMAP_DATA, &img, &mh) != ESP_OK) {
            read = "mmap_failed";
        } else {
            uint8_t h[32];
            mbedtls_sha256_context ctx;
            mbedtls_sha256_init(&ctx);
            mbedtls_sha256_starts(&ctx, 0);
            mbedtls_sha256_update(&ctx, img, run_len - 32);
            mbedtls_sha256_finish(&ctx, h);
            mbedtls_sha256_free(&ctx);
            esp_partition_munmap(mh);
            read = memcmp(h, run_digest, 32) ? "bad" : "ok";
        }
        ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    }
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI("test", "fwinfo part=%s state=%s len=%lu sha=%02x%02x%02x%02x version=%s project=%s read=%s read_ms=%lu "
             "servable=%d stack_free=%u", run_part ? run_part->label : "?", run_state_name, (unsigned long)run_len,
             run_digest[0], run_digest[1], run_digest[2], run_digest[3], d->version, d->project_name, read,
             (unsigned long)ms, (int)img_servable, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

// fwloop: this board's sender output fed straight into its receiver (packets, CRC, one injected CRC error, the
// first-sector check, esp_ota_begin/write/end) into the other slot. The boot slot is not changed.
static void do_loop(void)
{
    if (rx.state != R_IDLE) { ESP_LOGW("test", "error fwloop: busy"); return; }
    rx.part = esp_ota_get_next_update_partition(NULL);
    if (!run_len || !rx.part || !ensure_ring()) { ESP_LOGW("test", "error fwloop: no image, slot or ring"); return; }
    const void *img;
    esp_partition_mmap_handle_t mh;
    if (esp_partition_mmap(run_part, 0, run_len, ESP_PARTITION_MMAP_DATA, &img, &mh) != ESP_OK) {
        ESP_LOGW("test", "error fwloop: mmap");
        return;
    }
    uint32_t q;
    while (xQueueReceive(secq, &q, 0) == pdTRUE) {}
    memset(rx.peer_addr, 0, 6);
    rx.forced = true;                                 // same version: skip the version checks
    rx.loop = tx.loop = true;
    rx.len = run_len;
    rx.nsec = (run_len + SECTOR - 1) / SECTOR;
    memcpy(rx.ver, own_ver, 4);
    rx.cur = rx.off = 0;
    rx.seq = 0;
    rx.resync = false;
    rx.nacks = 0;
    rx.next_write = rx.logged = rx.write_us_max = 0;
    rx.done_ok = false;
    for (int i = 0; i < RING; i++) rx.slot_full[i] = false;
    rx.state = R_LOOP;
    rx.active = true;
    progress = 0;
    loop_corrupt = true;
    rx.t0_us = esp_timer_get_time();
    xEventGroupClearBits(eg, B_ALL);
    const char *why = tx_stream(img, run_len, PKT_MAX - 3, emit_loop);
    int64_t us = esp_timer_get_time() - rx.t0_us;
    loop_drain();
    tx.loop = false;
    esp_partition_munmap(mh);
    if (!why && !rx.done_ok) why = "not verified";
    if (rx.state != R_IDLE) rx_stop(false);
    char s[12], r[12];
    fmt_rate(s, sizeof(s), r, sizeof(r), run_len, us);
    if (why) ESP_LOGW("test", "error fwloop: %s", why);
    else
        ESP_LOGI("test", "fwloop ok part=%s len=%lu sectors=%lu crc_errors=%lu resent=%lu s=%s kbps=%s write_max_ms=%lu "
                 "stack_free=%u", rx.part->label, (unsigned long)run_len, (unsigned long)rx.nsec,
                 (unsigned long)rx.nacks, (unsigned long)tx.resent, s, r, (unsigned long)(rx.write_us_max / 1000),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void do_confirm(void)
{
    if (!img_pending) return;
    bool disp = display_lock(2000);
    if (disp) display_unlock();
    if (!disp || !mesh_synced()) {
        ESP_LOGW(TAG, "not confirming yet (display %s, bluetooth %s): again in 10 s", disp ? "up" : "stuck",
                 mesh_synced() ? "synced" : "not synced");
        esp_timer_start_once(confirm_timer, 10 * 1000000ULL);
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) { ESP_LOGW(TAG, "confirm failed: %s", esp_err_to_name(err)); return; }
    img_pending = false;
    img_servable = true;
    run_state_name = "valid";
    ESP_LOGI(TAG, "confirmed %s", esp_app_get_description()->version);
    mesh_refresh();                                   // the servable bit goes on
}

static void on_target(void)
{
    uint8_t f = forced;
    noted_missing = noted_devmode = false;
    if (f == MP_TARGET_NONE) {
        if (rx.state == R_WANT || rx.state == R_LINKED) { ESP_LOGI(TAG, "pull cancelled"); rx_stop(false); }
        return;
    }
    if (mesh_synced() && f == mesh_own_origin()) {
        ESP_LOGW(TAG, "target is self");
        forced = MP_TARGET_NONE;
        return;
    }
    if (rx.state == R_LINKED) { ESP_LOGI(TAG, "busy: fwpull %02x applies after this pull", f); return; }
    if (rx.state == R_WANT) rx_stop(false);
    backoff_until = now_ms();                         // an explicit fwpull does not wait for the back-off
}

static void on_event(uint32_t e)
{
    uint32_t a = e & 0xFFFFFF;
    switch (e >> 24) {
    case E_TARGET: on_target(); break;
    case E_PCONN: rx_on_connect((uint16_t)a); break;
    case E_PDISC:
        if (rx.state == R_LINKED && a == rx.conn) rx_fail("link lost");
        break;
    case E_START: {
        char ad[18];
        fmt_addr(ad, sizeof(ad), rx.peer_addr);
        rx.t0_us = esp_timer_get_time();
        ESP_LOGI(TAG, "receiving %lu bytes from %s (mtu %u)", (unsigned long)rx.len, ad, ble_att_mtu(rx.conn));
        break;
    }
    case E_RXERR: if (rx.state != R_IDLE) rx_fail("%s", rxe_text[a < 4 ? a : 0]); break;
    case E_INFO: do_info(); break;
    case E_LOOP: do_loop(); break;
    case E_CONFIRM: do_confirm(); break;
    }
}

static void fw_task(void *arg)
{
    uint32_t next_poll = now_ms() + POLL_MS;
    for (;;) {
        uint32_t e;
        if (rx.state == R_LINKED) {
            if (xQueueReceive(secq, &e, pdMS_TO_TICKS(200)) == pdTRUE) {
                rx_on_sector_event(e);
                while (xQueueReceive(secq, &e, 0) == pdTRUE) rx_on_sector_event(e);
            }
            while (xQueueReceive(evq, &e, 0) == pdTRUE) on_event(e);
        } else {
            int32_t wait = (int32_t)(next_poll - now_ms());
            if (xQueueReceive(evq, &e, pdMS_TO_TICKS(wait > 0 ? wait : 0)) == pdTRUE) on_event(e);
        }
        if ((int32_t)(now_ms() - next_poll) >= 0) {
            next_poll = now_ms() + POLL_MS;
            int n = mesh_peers(peers, MP_MAX_PEERS);
            rx_poll(n);
            if (rx.state == R_IDLE) tx_poll(n);
        }
    }
}

static void cmd_fwinfo(int argc, char **argv) { post(evq, EV(E_INFO, 0)); }
static void cmd_fwloop(int argc, char **argv) { post(evq, EV(E_LOOP, 0)); }
static void confirm_cb(void *arg) { post(evq, EV(E_CONFIRM, 0)); }

// ======================================================================================================
// API
// ======================================================================================================

void mesh_fw_init(void)
{
    size_t free0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const esp_app_desc_t *d = esp_app_get_description();
    mp_ver_from_string(d->version, own_ver);
    run_part = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    bool have_state = run_part && esp_ota_get_state_partition(run_part, &st) == ESP_OK;
    if (have_state) {
        run_state_name = st == ESP_OTA_IMG_VALID ? "valid" : st == ESP_OTA_IMG_UNDEFINED ? "undefined"
                       : st == ESP_OTA_IMG_PENDING_VERIFY ? "pending" : st == ESP_OTA_IMG_NEW ? "new"
                       : st == ESP_OTA_IMG_INVALID ? "invalid" : "aborted";
    }
    img_servable = !have_state || st == ESP_OTA_IMG_VALID || st == ESP_OTA_IMG_UNDEFINED;
    img_pending = have_state && st == ESP_OTA_IMG_PENDING_VERIFY;
    if (run_part) {
        esp_partition_pos_t pos = { .offset = run_part->address, .size = run_part->size };
        esp_image_metadata_t md;
        if (esp_image_get_metadata(&pos, &md) == ESP_OK) {
            run_len = md.image_len;
            run_hashed = md.image.hash_appended;
            memcpy(run_digest, md.image_digest, 32);
        }
    }
    if (!run_len) { img_servable = false; ESP_LOGW(TAG, "no image metadata: not serving"); }
    const esp_partition_t *inv = esp_ota_get_last_invalid_partition();
    esp_app_desc_t bd;
    if (inv && esp_ota_get_partition_description(inv, &bd) == ESP_OK) {
        memcpy(bad_version, bd.version, sizeof(bad_version));
        bad_version[sizeof(bad_version) - 1] = 0;
        mp_ver_from_string(bad_version, bad_ver);
        ESP_LOGW(TAG, "%s rolled back on %s: never pulled again", bad_version, inv->label);
    }

    evq = xQueueCreate(16, sizeof(uint32_t));
    secq = xQueueCreate(16, sizeof(uint32_t));
    eg = xEventGroupCreate();
    if (!evq || !secq || !eg) { ESP_LOGE(TAG, "no memory"); return; }
    int rc = ble_gatts_count_cfg(svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(svcs);
    if (rc) ESP_LOGE(TAG, "GATT service rc=%d", rc);
    mesh_set_gap_hook(gap_hook);
    testcon_register("fwinfo", "fwinfo", cmd_fwinfo);
    testcon_register("fwloop", "fwloop (own image into the other slot, boot unchanged)", cmd_fwloop);
    if (xTaskCreatePinnedToCore(fw_task, "fw", 5120, NULL, 4, &fw_task_h, 0) != pdPASS) {
        ESP_LOGE(TAG, "no task");
        return;
    }
    const esp_timer_create_args_t ta = { .callback = confirm_cb, .name = "fw_confirm" };
    if (esp_timer_create(&ta, &confirm_timer) == ESP_OK && img_pending) {
        ESP_LOGI(TAG, "pending verify, confirming in 60 s");
        esp_timer_start_once(confirm_timer, CONFIRM_MS * 1000ULL);
    }
    size_t free1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "image %s %s %s %lu bytes, %s; init took %u bytes of internal RAM",
             run_part ? run_part->label : "?", run_state_name, d->version, (unsigned long)run_len,
             img_servable ? "servable" : "not servable", (unsigned)(free0 > free1 ? free0 - free1 : 0));
}

void mesh_fw_set_target(uint8_t origin)
{
    forced = origin;
    post(evq, EV(E_TARGET, origin));
}

int mesh_fw_progress(void) { return progress; }
