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
// Link set-up runs one Link Layer procedure at a time (data length, then 2M PHY, then the interval, then the MTU)
// and logs what each one granted: "fw: link ..." lines on both boards.
// The rate is set by the SENDER'S SCAN DUTY, not by the link: with the flood's continuous 100/100 ms scan open, the
// controller gives the connection one 251-byte packet per 7.5 ms event (22 KB/s, measured 2026-10-31 with 2M PHY,
// DLE 251 and MTU 512 all granted); scan 50 ms of 100 gave 40 KB/s, 30 ms 52 KB/s, no scan 75 KB/s. Advertising,
// the window, the event length and the order of the link procedures changed nothing. So the sender scans
// XFER_SCAN_MS of every 100 ms during a transfer (the flood still arrives, slower) and the receiver pauses its scan.
// `fwtune` (test console) changes the sender's window, scan duty and connection parameters in RAM and can stop a
// transfer after N KB ("probe": the dev peer retries after its 30 s back-off), so link settings can be compared
// without re-flashing the receiving board.
//
//
// Secure updates (the admin board, fw_auth.h for the byte layouts): a board OUT of dev mode with a trusted admin key
// (console "trust") pulls only from a peer advertising MP_F_ADMIN, and only a signed image. After the link is up the
// admin sender asks for a challenge (CTRL 04); the receiver notifies a fresh 32-byte nonce (ACK, sector 0xFFFE,
// status 3, 36 bytes); the sender answers with CTRL 03, the proof: nonce, image length, image digest, version and an
// ECDSA P-256 signature (raw r || s) over SHA-256 of those 100 bytes, made with its private key (NVS "adm_priv");
// the receiver checks the nonce (single use, 10 s), the version (strictly newer), the signature with the trusted key,
// answers a verdict (ACK, sector 0xFFFE, status 4) and then accepts only the START with the signed length and digest;
// at the end its own SHA-256 of what it wrote must equal the signed digest. Without a valid proof it refuses
// ("fw: refused <addr>: no proof | bad signature | ...") and disconnects. Dev-mode boards pull as before, from anyone;
// a proof is checked when one comes (an admin sender always offers one) but not required. Why the sender asks
// instead of the receiver pushing the nonce at once: ACK notifications reach the sender without any subscription
// (ble_gatts_notify_custom ignores the CCCD) through BLE_GAP_EVENT_NOTIFY_RX, filtered on the ACK handle, which the
// sender only knows after discovery; and a v0.3.0 sender treats any unknown ACK status as an abort, so a dev-mode
// receiver must never notify a challenge nobody asked for.
//
// Threads: the GAP hook and the GATT callbacks run in the NimBLE host task and only copy and post; every NimBLE
// procedure, flash write, mesh_* call, ECDSA operation and log line is in the "fw" task (core 0, internal stack,
// FW_STACK: fwloop, which runs esp_ota_end, left 732 bytes of 4 KB; the ECDSA sign and verify run here too).
#include "mesh_fw.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/bignum.h"
#include "mbedtls/md.h"
#include "host/ble_hs.h"
#include "host/ble_esp_gap.h"
#include "testcon.h"
#include "board.h"
#include "config.h"
#include "fw_auth.h"
#include "mesh.h"
#include "mesh_proto.h"
#include "ui.h"

static const char *TAG = "fw";

#define SECTOR          4096
#define RING            8           // receiver: sectors buffered in PSRAM
#define WINDOW          4           // sender: sectors sent beyond the last ACKed one
#define XFER_SCAN_MS    30          // sender's scan window (of 100 ms) during a transfer: 52 KB/s measured, 40 at 50 ms
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
#define FW_STACK        6144        // 5 KB before the ECDSA; 8 KB left 3.9 KB after fwloop + selftest + serving (fwinfo, admin selftest: stack_free)
#define CHAL_MAX        3           // challenges a receiver issues per link
#define VERDICT_MS      5000        // sender: waiting for the receiver's verdict on the proof

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
enum { E_TARGET = 1, E_PCONN, E_PDISC, E_START, E_SECTOR, E_NACK, E_RXERR, E_INFO, E_LOOP, E_CONFIRM, E_LINK, E_TUNE,
       E_CHAL, E_PROOF, E_ADMIN, E_TRUST };
// E_LINK argument: kind << 20 | 20 bits of payload (receiver side: what the link granted, logged by the fw task)
enum { LK_PHY = 1, LK_DLE, LK_UPD, LK_MTU };
#define EV(t, a) (((uint32_t)(t) << 24) | ((uint32_t)(a) & 0xFFFFFF))
enum { RXE_ABORT = 1, RXE_NOT_NEWER, RXE_BUSY, RXE_NO_PROOF, RXE_UNSIGNED_START };
static const char *const rxe_text[] = { "", "the sender aborted", "offered version is not newer",
                                        "ring slot still busy (sender ignored the window)", "no proof",
                                        "START does not match the signed image" };
#define RXE_N ((int)(sizeof(rxe_text) / sizeof(rxe_text[0])))

// sender link bits (host task -> fw task)
#define B_CONN      BIT0
#define B_CONN_FAIL BIT1
#define B_DISC      BIT2
#define B_MTU       BIT3
#define B_PHY       BIT4
#define B_DLE       BIT5
#define B_GATT      BIT6
#define B_ACK       BIT7
#define B_UPD       BIT8
#define B_CHAL      BIT9            // a challenge arrived (tx.nonce)
#define B_VERDICT   BIT10           // the receiver's verdict on the proof (tx.verdict)
#define B_ALL       0x7FF

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
    // secure updates
    bool signed_pull;               // chosen by the secure rule (out of dev mode, from an admin peer)
    volatile bool need_proof;       // out of dev mode at connect: START needs a verified proof first
    volatile bool proof_ok;         // the proof was verified: START must carry its length and digest (host task reads)
    fwa_proof_t proof;              // the signed values, written by the fw task before proof_ok
    fwa_nonce_t nonce;              // the live challenge (fw task)
    uint8_t chals;                  // challenges issued on this link
    mbedtls_sha256_context sha;     // the receiver's own digest of the bytes written (signed pulls)
    bool sha_on;
} rx = { .conn = BLE_HS_CONN_HANDLE_NONE };
EXT_RAM_BSS_ATTR static uint8_t proof_in[FWA_PROOF_LEN];   // CTRL 03 as written (host task -> fw task)
static volatile bool proof_busy;
static uint8_t *ring;               // RING * SECTOR in PSRAM, allocated on the first pull, kept
static uint8_t rxbuf[PKT_MAX + 4];  // host task only
static uint8_t pconn_addr[6];       // the central of the last peripheral connection (host task -> fw task)
static uint32_t backoff_until;
static bool noted_missing, noted_devmode;

// ---- sender ----
static struct {
    volatile uint16_t conn;
    volatile int conn_status, disc_reason, gatt_status;
    volatile uint16_t mtu, dle, dle_rx, dle_time;
    volatile uint8_t phy, phy_rx;
    volatile int phy_status, upd_status;
    uint16_t svc_start, svc_end, h_ctrl, h_data, h_ack;
    volatile int32_t acked, nack;
    volatile bool abort_rx, loop;
    volatile int done;
    uint32_t enomem, resent, enomem_ms, wait_ms;       // back-offs, and time spent waiting for the window to open
    int64_t t_stream_us;
    bool probed;                                      // the stream stopped at tune.probe_kb
    uint8_t nonce[FWA_NONCE_LEN];                     // the receiver's challenge (host task -> fw task)
    volatile int verdict;                             // the receiver's verdict on our proof (FWA_V_*)
} tx = { .conn = BLE_HS_CONN_HANDLE_NONE };

// fwtune knobs (RAM; the defaults are the release's behaviour). Read by the fw task at the start of a transfer.
static struct {
    volatile uint8_t window;        // sectors sent beyond the last ACKed one, 1..RING
    volatile uint16_t probe_kb;     // sender: stop after this many KB and abort (0 = the whole image)
    volatile uint8_t scan;          // sender's scan window during a transfer, ms of each 100 ms (0 = scan paused, 100 = the flood's)
    volatile uint8_t adv;           // sender keeps advertising during a transfer (1) or pauses it (0)
    volatile uint8_t rxscan;        // receiver keeps scanning during a pull (1) or pauses its scan (0)
    volatile uint16_t itvl, min_ce, max_ce;   // connection interval and event length, 1.25 / 0.625 ms units
    volatile uint8_t seq;           // link set-up one procedure at a time (1) or all at once as rc.1 did (0)
} tune = { .window = WINDOW, .probe_kb = 0, .scan = XFER_SCAN_MS, .adv = 1, .rxscan = 0, .itvl = 6, .min_ce = 12, .max_ce = 24,
           .seq = 1 };
static const char *const PROBE_DONE = "probe done";
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
// ECDSA P-256 (fw task only): key pair, sign, verify. mbedTLS; randomness from esp_fill_random through a CTR-DRBG
// seeded by esp_fill_random. Private keys live in a stack buffer or an mbedtls_mpi only while used, then zeroed
// (mbedtls_mpi_free zeroes the limbs).
// ======================================================================================================

EXT_RAM_BSS_ATTR static mbedtls_ctr_drbg_context drbg;   // PSRAM: internal RAM is the scarce kind (L185)
static bool drbg_ok;

static int entropy_cb(void *ctx, unsigned char *out, size_t n)
{
    esp_fill_random(out, n);
    return 0;
}

static int rng(void *ctx, unsigned char *out, size_t n) { return mbedtls_ctr_drbg_random(&drbg, out, n); }

static bool drbg_ready(void)
{
    if (drbg_ok) return true;
    static const char pers[] = "colormesh fw admin";
    mbedtls_ctr_drbg_init(&drbg);
    int rc = mbedtls_ctr_drbg_seed(&drbg, entropy_cb, NULL, (const unsigned char *)pers, sizeof(pers) - 1);
    if (rc) { ESP_LOGW(TAG, "drbg seed rc=-0x%04x", -rc); mbedtls_ctr_drbg_free(&drbg); return false; }
    drbg_ok = true;
    return true;
}

// A new key pair: priv (32 bytes, big-endian), pub (X || Y). 0 on success
static int ec_keygen(uint8_t priv[FWA_PRIV_LEN], uint8_t pub[FWA_PUB_LEN])
{
    if (!drbg_ready()) return -1;
    mbedtls_ecp_keypair kp;
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_keypair_init(&kp);
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    uint8_t buf[1 + FWA_PUB_LEN];
    size_t olen = 0;
    int rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &kp, rng, NULL);
    if (!rc) rc = mbedtls_ecp_export(&kp, &grp, &d, &q);
    if (!rc) rc = mbedtls_mpi_write_binary(&d, priv, FWA_PRIV_LEN);
    if (!rc) rc = mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, buf, sizeof(buf));
    if (!rc && (olen != sizeof(buf) || buf[0] != 0x04)) rc = -1;
    if (!rc) memcpy(pub, buf + 1, FWA_PUB_LEN);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    mbedtls_ecp_keypair_free(&kp);
    if (rc) fwa_zero(priv, FWA_PRIV_LEN);
    return rc;
}

static int ec_load_pub(mbedtls_ecp_group *grp, mbedtls_ecp_point *q, const uint8_t pub[FWA_PUB_LEN])
{
    uint8_t buf[1 + FWA_PUB_LEN];
    buf[0] = 0x04;
    memcpy(buf + 1, pub, FWA_PUB_LEN);
    int rc = mbedtls_ecp_group_load(grp, MBEDTLS_ECP_DP_SECP256R1);
    if (!rc) rc = mbedtls_ecp_point_read_binary(grp, q, buf, sizeof(buf));
    if (!rc) rc = mbedtls_ecp_check_pubkey(grp, q);
    return rc;
}

// A point on P-256 (the "trust" check)
static bool ec_pub_valid(const uint8_t pub[FWA_PUB_LEN])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    int rc = ec_load_pub(&grp, &q, pub);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return rc == 0;
}

// sig = r || s over hash with the stored private key (deterministic ECDSA, RFC 6979, SHA-256). 0 on success
static int ec_sign(const uint8_t hash[32], uint8_t sig[FWA_SIG_LEN])
{
    uint8_t priv[FWA_PRIV_LEN];
    if (!config_admin_priv(priv)) return -1;
    if (!drbg_ready()) { fwa_zero(priv, sizeof(priv)); return -1; }
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (!rc) rc = mbedtls_mpi_read_binary(&d, priv, sizeof(priv));
    fwa_zero(priv, sizeof(priv));
    if (!rc) rc = mbedtls_ecdsa_sign_det_ext(&grp, &r, &s, &d, hash, 32, MBEDTLS_MD_SHA256, rng, NULL);
    if (!rc) rc = mbedtls_mpi_write_binary(&r, sig, 32);
    if (!rc) rc = mbedtls_mpi_write_binary(&s, sig + 32, 32);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

// 0 when sig (r || s) is a valid signature of hash by pub
static int ec_verify(const uint8_t pub[FWA_PUB_LEN], const uint8_t hash[32], const uint8_t sig[FWA_SIG_LEN])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int rc = ec_load_pub(&grp, &q, pub);
    if (!rc) rc = mbedtls_mpi_read_binary(&r, sig, 32);
    if (!rc) rc = mbedtls_mpi_read_binary(&s, sig + 32, 32);
    if (!rc) rc = mbedtls_ecdsa_verify(&grp, hash, 32, &q, &r, &s);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

static void proof_hash(const fwa_proof_t *p, uint8_t hash[32])
{
    uint8_t m[FWA_MSG_LEN];
    fwa_proof_message(m, p);
    mbedtls_sha256(m, sizeof(m), hash, 0);
}

static uint32_t ms_since(int64_t t0_us) { return (uint32_t)((esp_timer_get_time() - t0_us) / 1000); }

// The sender's proof for a challenge: this board's image, version and signature. 0 on success; sign_ms how long
static int proof_make(fwa_proof_t *p, const uint8_t nonce[FWA_NONCE_LEN], uint32_t *sign_ms)
{
    memset(p, 0, sizeof(*p));
    memcpy(p->nonce, nonce, FWA_NONCE_LEN);
    p->image_len = run_len;
    memcpy(p->digest, run_digest, FWA_DIGEST_LEN);
    strlcpy(p->version, esp_app_get_description()->version, sizeof(p->version));
    uint8_t h[32];
    proof_hash(p, h);
    int64_t t0 = esp_timer_get_time();
    int rc = ec_sign(h, p->sig);
    if (sign_ms) *sign_ms = ms_since(t0);
    return rc;
}

// The receiver's judgement of a proof: nonce (taken), version (strictly newer than own), signature with key (NULL:
// none trusted). FWA_V_* ; *why for the log; verify_ms how long the ECDSA verify took
static uint8_t proof_judge(const fwa_proof_t *p, const uint8_t own[4], const uint8_t *key, const char **why,
                           uint32_t *verify_ms)
{
    if (verify_ms) *verify_ms = 0;
    fwa_result_t r = fwa_check(&rx.nonce, p, own, now_ms());
    if (r != FWA_OK) { *why = fwa_result_text(r); return fwa_result_verdict(r); }
    if (!key) { *why = "no trusted key"; return FWA_V_UNCHECKED; }
    uint8_t h[32];
    proof_hash(p, h);
    int64_t t0 = esp_timer_get_time();
    int rc = ec_verify(key, h, p->sig);
    if (verify_ms) *verify_ms = ms_since(t0);
    *why = rc ? "bad signature" : "ok";
    return rc ? FWA_V_BAD_SIG : FWA_V_OK;
}

static void short_of(const uint8_t pub[FWA_PUB_LEN], char out[FWA_SHORT_LEN + 1]) { fwa_short_id(pub, out); }

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
    uint8_t b[FWA_PROOF_LEN];                         // the longest command (START is 41 bytes)
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), &n) != 0 || n < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (rx.state != R_LINKED || conn != rx.conn) return BLE_ATT_ERR_UNLIKELY;
    if (b[0] == CTRL_ABORT) {
        rx.active = false;
        post(evq, EV(E_RXERR, RXE_ABORT));
        return 0;
    }
    if (b[0] == FWA_CTRL_CHALLENGE) {                 // an admin sender asks for a nonce (the fw task notifies it)
        if (n != 1 || rx.active) return BLE_ATT_ERR_UNLIKELY;
        post(evq, EV(E_CHAL, 0));
        return 0;
    }
    if (b[0] == FWA_CTRL_PROOF) {                     // judged by the fw task, which notifies the verdict
        if (n != FWA_PROOF_LEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (rx.active || proof_busy) return BLE_ATT_ERR_UNLIKELY;
        memcpy(proof_in, b, FWA_PROOF_LEN);
        proof_busy = true;
        post(evq, EV(E_PROOF, 0));
        return 0;
    }
    if (b[0] != CTRL_START || n != START_LEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint32_t len = b[1] | (uint32_t)b[2] << 8 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 24;
    if (rx.active || !ring || !rx.part || len < SECTOR || len > rx.part->size) return BLE_ATT_ERR_UNLIKELY;
    if (rx.need_proof && !rx.proof_ok) {              // out of dev mode: no START without a verified proof
        post(evq, EV(E_RXERR, RXE_NO_PROOF));
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (rx.proof_ok && !fwa_start_matches(&rx.proof, len, b + 5)) {
        post(evq, EV(E_RXERR, RXE_UNSIGNED_START));
        return BLE_ATT_ERR_UNLIKELY;
    }
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
    } else if (e->type == BLE_GAP_EVENT_PHY_UPDATE_COMPLETE && e->phy_updated.conn_handle == rx.conn) {
        post(evq, EV(E_LINK, LK_PHY << 20 | (e->phy_updated.status & 0xFF) << 8 | (e->phy_updated.tx_phy & 0xF) << 4
                            | (e->phy_updated.rx_phy & 0xF)));
    } else if (e->type == BLE_GAP_EVENT_DATA_LEN_CHG && e->data_len_chg.conn_handle == rx.conn) {
        post(evq, EV(E_LINK, LK_DLE << 20 | (e->data_len_chg.max_tx_octets & 0x3FF) << 10
                            | (e->data_len_chg.max_rx_octets & 0x3FF)));
    } else if (e->type == BLE_GAP_EVENT_CONN_UPDATE && e->conn_update.conn_handle == rx.conn) {
        post(evq, EV(E_LINK, LK_UPD << 20 | (e->conn_update.status & 0xFFFF)));
    } else if (e->type == BLE_GAP_EVENT_MTU && e->mtu.conn_handle == rx.conn) {
        post(evq, EV(E_LINK, LK_MTU << 20 | (e->mtu.value & 0xFFFF)));
    }
}

static const char *phy_name(unsigned p) { return p == 2 ? "2M" : p == 1 ? "1M" : p == 3 ? "coded" : "?"; }

// Receiver side: one line per thing the link granted (fw task)
static void rx_on_link(uint32_t a)
{
    uint32_t v = a & 0xFFFFF;
    struct ble_gap_conn_desc d;
    int itvl = rx.conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(rx.conn, &d) == 0 ? d.conn_itvl : 0;
    switch (a >> 20) {
    case LK_PHY: ESP_LOGI(TAG, "link phy tx=%s rx=%s (status %lu)", phy_name(v >> 4 & 0xF), phy_name(v & 0xF),
                          (unsigned long)(v >> 8 & 0xFF)); break;
    case LK_DLE: ESP_LOGI(TAG, "link dle tx=%lu rx=%lu", (unsigned long)(v >> 10 & 0x3FF), (unsigned long)(v & 0x3FF)); break;
    case LK_UPD: ESP_LOGI(TAG, "link itvl=%d.%02d ms (update status %lu)", itvl * 125 / 100, itvl * 125 % 100, (unsigned long)v); break;
    case LK_MTU: ESP_LOGI(TAG, "link mtu=%lu", (unsigned long)v); break;
    }
}

// ======================================================================================================
// Receiver: fw task
// ======================================================================================================

static void tx_on_ack(uint32_t idx, uint8_t st);

static int notify_ack(const uint8_t *b, int len)
{
    for (int i = 0; i < 200; i++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(b, len);
        int rc = om ? ble_gatts_notify_custom(rx.conn, ack_handle, om) : BLE_HS_ENOMEM;   // frees om
        if (rc != BLE_HS_ENOMEM) return rc;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return BLE_HS_ENOMEM;
}

static int send_ack(uint16_t idx, uint8_t st)
{
    if (rx.loop) { tx_on_ack(idx, st); return 0; }
    uint8_t b[ACK_LEN] = { (uint8_t)idx, (uint8_t)(idx >> 8), st };
    return notify_ack(b, sizeof(b));
}

static void rx_auth_reset(void)
{
    rx.proof_ok = false;
    rx.need_proof = false;
    rx.chals = 0;
    fwa_nonce_clear(&rx.nonce);
    fwa_zero(&rx.proof, sizeof(rx.proof));
    if (rx.sha_on) { mbedtls_sha256_free(&rx.sha); rx.sha_on = false; }
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
        if (linked) mesh_scan_pause(false);
    }
    if (rx.loop) send_ack(ACK_DONE, ST_ABORT);        // fwloop: the sender side stops at once
    rx.conn = BLE_HS_CONN_HANDLE_NONE;
    rx.state = R_IDLE;
    rx.loop = false;
    rx.signed_pull = false;
    rx_auth_reset();
    progress = -1;
    if (backoff) backoff_until = now_ms() + BACKOFF_MS;
}

// A refusal (no proof, bad signature, ...): "fw: refused <addr>: <why>", the link closed, 30 s before the next try
static void rx_refuse(const char *why)
{
    char a[18];
    fmt_addr(a, sizeof(a), rx.peer_addr);
    ESP_LOGW(TAG, "refused %s: %s", a, why);
    rx_stop(true);
}

static char last_fail[80];           // the last rx_fail reason (fwloop reports it)

static void rx_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_fail, sizeof(last_fail), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "failed: %s", last_fail);
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
    if (rx.proof_ok && strncmp(rx.version, rx.proof.version, sizeof(rx.version)))
        return "image version is not the signed one";
    if (rx.forced) return NULL;
    uint8_t v[4];
    mp_ver_from_string(rx.version, v);
    if (mp_ver_cmp(v, own_ver) <= 0) return "image version is not newer";
    if (bad_version[0] && !strncmp(rx.version, bad_version, sizeof(bad_version))) return "that version rolled back here";
    return NULL;
}

static void rx_finish(void)
{
    if (rx.proof_ok) {                                // signed: what was written must be what the admin signed
        uint8_t h[32];
        bool ok = rx.sha_on && mbedtls_sha256_finish(&rx.sha, h) == 0 && fwa_ct_equal(h, rx.proof.digest, 32);
        if (rx.sha_on) { mbedtls_sha256_free(&rx.sha); rx.sha_on = false; }
        if (!ok) { rx_fail("digest mismatch"); return; }   // rx_stop: esp_ota_abort, 30 s back-off
    }
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
    ESP_LOGI(TAG, "pulled %s from %s in %s s (%s KB/s, %lu resent%s), restarting", rx.version, a, s, r,
             (unsigned long)rx.nacks, rx.proof_ok ? ", signed, digest ok" : "");
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
        if (rx.proof_ok) {                            // the image digest covers all but its last 32 bytes
            if (rx.sha_on) mbedtls_sha256_free(&rx.sha);
            mbedtls_sha256_init(&rx.sha);
            rx.sha_on = mbedtls_sha256_starts(&rx.sha, 0) == 0;
        }
    }
    if (rx.sha_on) {
        uint32_t o = idx * SECTOR, hl = rx.len - FWA_DIGEST_LEN;
        if (o < hl) mbedtls_sha256_update(&rx.sha, s, n < hl - o ? n : hl - o);
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
    case E_RXERR: if (rx.state != R_IDLE) rx_fail("%s", rxe_text[a < RXE_N ? a : 0]); break;
    }
}

// Every second: choose a peer to pull from (dev mode), watch the timeouts
static void rx_poll(int n)
{
    uint32_t now = now_ms();
    const app_cfg_t *c = config_get();
    bool dev = c->dev_mode;
    bool secure = !dev && c->has_trusted;             // out of dev mode: signed pulls from an admin peer only
    if (rx.state == R_WANT) {
        if (!dev && !(secure && rx.signed_pull)) { ESP_LOGI(TAG, "pull stopped: dev mode off"); rx_stop(false); return; }
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
    if (!dev && (f != MP_TARGET_NONE || !secure)) {   // fwpull (unsigned, any peer) is a dev-mode tool
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
            if (!dev && !p->admin) continue;          // out of dev mode: only an admin can serve (signed)
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
    rx.signed_pull = !dev;
    rx.loop = false;
    rx.want_ms = now;
    rx.state = R_WANT;
    mesh_set_target(best->origin);
    mesh_set_connectable(true);
    ESP_LOGI(TAG, "want %s from %s%s", v, a, rx.forced ? " (fwpull)" : rx.signed_pull ? " (signed, admin)" : "");
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
    if (!tune.rxscan) mesh_scan_pause(true);          // the radio is the link's until the pull ends
    memcpy(rx.peer_addr, pconn_addr, 6);
    rx_auth_reset();
    rx.need_proof = !config_get()->dev_mode;          // out of dev mode: START only after a verified proof
    rx.conn = conn;
    rx.active = false;
    rx.next_write = 0;
    rx.logged = 0;
    rx.write_us_max = 0;
    rx.last_ms = now_ms();
    rx.state = R_LINKED;
    progress = 0;
    ESP_LOGI(TAG, "linked to %s%s", a, rx.need_proof ? " (a signed image is required)" : "");
}

// CTRL 04: a fresh nonce, notified as the 36-byte challenge (the sender signs it into its proof)
static void rx_on_challenge_request(void)
{
    if (rx.state != R_LINKED || rx.active) return;
    if (rx.chals >= CHAL_MAX) { ESP_LOGW(TAG, "challenge refused: %d already on this link", CHAL_MAX); return; }
    rx.chals++;
    uint8_t n[FWA_NONCE_LEN], b[FWA_CHAL_LEN];
    esp_fill_random(n, sizeof(n));
    fwa_nonce_issue(&rx.nonce, n, now_ms());
    fwa_challenge_encode(b, n);
    int rc = notify_ack(b, sizeof(b));
    fwa_zero(n, sizeof(n));
    fwa_zero(b, sizeof(b));
    if (rc) { ESP_LOGW(TAG, "challenge notify rc=%d", rc); return; }
    ESP_LOGI(TAG, "challenge sent");
}

static void rx_send_verdict(uint8_t v)
{
    uint8_t b[FWA_VERDICT_LEN];
    fwa_verdict_encode(b, v);
    notify_ack(b, sizeof(b));
}

// CTRL 03: the proof. Out of dev mode anything but a good proof ends the link (30 s back-off); in dev mode a proof
// that does not check out is set aside and the pull goes on unsigned, as every dev-mode pull did before
static void rx_on_proof(void)
{
    fwa_proof_t p;
    bool parsed = fwa_proof_decode(proof_in, FWA_PROOF_LEN, &p);
    fwa_zero(proof_in, sizeof(proof_in));
    proof_busy = false;
    if (rx.state != R_LINKED || rx.active) return;
    char a[18];
    fmt_addr(a, sizeof(a), rx.peer_addr);
    const app_cfg_t *c = config_get();
    const char *why = "malformed proof";
    uint32_t vms = 0;
    uint8_t v = parsed ? proof_judge(&p, own_ver, c->has_trusted ? c->trusted : NULL, &why, &vms) : FWA_V_BAD_PROOF;
    if (v == FWA_V_OK) {
        rx.proof = p;                                 // before proof_ok: the host task reads it once proof_ok is set
        rx.proof_ok = true;
        char k[FWA_SHORT_LEN + 1];
        short_of(c->trusted, k);
        ESP_LOGI(TAG, "proof ok from %s (key %s, %s, %lu bytes, verify %lu ms)", a, k, p.version,
                 (unsigned long)p.image_len, (unsigned long)vms);
        rx_send_verdict(FWA_V_OK);
        return;
    }
    if (rx.need_proof) {
        rx_send_verdict(v);
        rx_refuse(why);                               // "fw: refused <addr>: bad signature" (or nonce, version...)
        return;
    }
    if (v == FWA_V_UNCHECKED) ESP_LOGI(TAG, "proof from %s not checked: %s (dev mode: the pull goes on unsigned)", a, why);
    else ESP_LOGW(TAG, "proof from %s not used: %s (dev mode: the pull goes on unsigned)", a, why);
    rx_send_verdict(FWA_V_UNCHECKED);
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
        tx.phy_status = e->phy_updated.status;
        if (e->phy_updated.status == 0) { tx.phy = e->phy_updated.tx_phy; tx.phy_rx = e->phy_updated.rx_phy; }
        xEventGroupSetBits(eg, B_PHY);
        break;
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        tx.dle = e->data_len_chg.max_tx_octets;
        tx.dle_rx = e->data_len_chg.max_rx_octets;
        tx.dle_time = e->data_len_chg.max_tx_time;
        xEventGroupSetBits(eg, B_DLE);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        tx.upd_status = e->conn_update.status;
        xEventGroupSetBits(eg, B_UPD);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (e->notify_rx.attr_handle == tx.h_ack && OS_MBUF_PKTLEN(e->notify_rx.om) >= 3) {
            uint8_t b[FWA_CHAL_LEN];
            uint16_t k = OS_MBUF_PKTLEN(e->notify_rx.om);
            if (k > sizeof(b)) k = sizeof(b);
            os_mbuf_copydata(e->notify_rx.om, 0, k, b);
            uint32_t idx = b[0] | (uint32_t)b[1] << 8;
            if (idx == FWA_SECTOR_AUTH) {             // the challenge or the verdict, never a sector ACK
                int v;
                if (fwa_challenge_decode(b, k, tx.nonce)) xEventGroupSetBits(eg, B_CHAL);
                else if ((v = fwa_verdict_decode(b, k)) >= 0) { tx.verdict = v; xEventGroupSetBits(eg, B_VERDICT); }
                fwa_zero(b, sizeof(b));
            } else {
                tx_on_ack(idx, b[2]);
            }
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
        tx.enomem_ms++;
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
    tx.enomem = tx.resent = tx.enomem_ms = tx.wait_ms = 0;
    tx.probed = false;
    tx.t_stream_us = esp_timer_get_time();
    for (;;) {
        // fwtune's window and probe are read live: a probe sent during a transfer stops it
        uint32_t window = tx.loop ? WINDOW : tune.window < 1 ? 1 : tune.window > RING ? RING : tune.window;
        uint32_t probe = tx.loop ? 0 : (uint32_t)tune.probe_kb * 1024;
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
                char s[12], r[12];
                fmt_rate(s, sizeof(s), r, sizeof(r), done, esp_timer_get_time() - tx.t_stream_us);
                ESP_LOGI(TAG, "sent %lu/%lu KB in %s s (%s KB/s)", (unsigned long)(done / 1024),
                         (unsigned long)(len / 1024), s, r);
            }
            if (probe && done >= probe) { tx.probed = true; return NULL; }
        }
        if (a >= (int32_t)nsec - 1) return NULL;
        if (next < nsec && (int32_t)next <= a + (int32_t)window) {
            int rc = tx_sector(img, len, next, dmax, emit);
            if (rc) return rc == BLE_HS_ENOTCONN ? "link lost" : "write failed";
            next++;
            if (tx.loop) loop_drain();
            continue;
        }
        if (now_ms() - last_ms > LINK_IDLE_MS) return "no ACK for 15 s";
        if (tx.loop) { loop_drain(); if (tx.acked == last_acked && !tx.abort_rx && tx.nack < 0) return "fwloop stalled"; continue; }
        uint32_t w0 = now_ms();
        xEventGroupWaitBits(eg, B_ACK | B_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
        tx.wait_ms += now_ms() - w0;
    }
}

static EventBits_t wait_bits(EventBits_t want, bool all, int ms)
{
    return xEventGroupWaitBits(eg, want | B_DISC, pdFALSE, all ? pdTRUE : pdFALSE, pdMS_TO_TICKS(ms));
}

static const char *const verdict_text[] = { "ok", "bad signature", "nonce refused", "version not newer",
                                            "malformed proof", "not checked" };

// An admin sender's proof, before START: ask for a challenge, sign it, wait for the verdict. NULL to go on with
// START (signed, or unsigned when the receiver is older or in dev mode without a trusted key); else why to stop.
static const char *tx_prove(uint16_t c, const char *a)
{
    const app_cfg_t *cf = config_get();
    if (!cf->admin || !cf->has_admin_key) return NULL;     // not the admin: START unsigned (a non-dev board refuses it)
    if (!run_hashed) { ESP_LOGW(TAG, "no proof for %s: the image has no appended SHA-256", a); return NULL; }
    xEventGroupClearBits(eg, B_CHAL | B_VERDICT | B_GATT);
    tx.verdict = -1;
    uint8_t q = FWA_CTRL_CHALLENGE;
    if (ble_gattc_write_flat(c, tx.h_ctrl, &q, 1, write_cb, NULL)) return "challenge request write";
    EventBits_t b = wait_bits(B_GATT, false, 5000);
    if (b & B_DISC) return "link lost";
    if (!(b & B_GATT)) return "no answer to the challenge request";
    if (tx.gatt_status) {                             // a v0.3.0 receiver: CTRL knows START and ABORT only
        ESP_LOGI(TAG, "%s takes no proof (older firmware): START unsigned", a);
        return NULL;
    }
    b = wait_bits(B_CHAL, false, 3000);
    if (b & B_DISC) return "link lost";
    if (!(b & B_CHAL)) return "no challenge";
    fwa_proof_t p;
    uint32_t sign_ms = 0;
    int rc = proof_make(&p, tx.nonce, &sign_ms);
    fwa_zero(tx.nonce, sizeof(tx.nonce));
    if (rc) { static char why[40]; snprintf(why, sizeof(why), "signing failed (-0x%04x)", -rc); return why; }
    uint8_t buf[FWA_PROOF_LEN];
    fwa_proof_encode(buf, &p);
    xEventGroupClearBits(eg, B_GATT);
    rc = ble_gattc_write_flat(c, tx.h_ctrl, buf, sizeof(buf), write_cb, NULL);
    fwa_zero(buf, sizeof(buf));
    if (rc) return "proof write";
    b = wait_bits(B_GATT, false, 5000);
    if (b & B_DISC) return "link lost";
    if (!(b & B_GATT) || tx.gatt_status) return "proof not taken";
    b = wait_bits(B_VERDICT, false, VERDICT_MS);
    if (!(b & B_VERDICT)) return b & B_DISC ? "link lost" : "no verdict on the proof";
    int v = tx.verdict;
    char k[FWA_SHORT_LEN + 1];
    short_of(cf->admin_pub, k);
    if (v == FWA_V_OK) {
        ESP_LOGI(TAG, "proof sent to %s: accepted (key %s, sign %lu ms)", a, k, (unsigned long)sign_ms);
        return NULL;
    }
    if (v == FWA_V_UNCHECKED) {
        ESP_LOGI(TAG, "proof sent to %s: not checked there (dev mode): START unsigned", a);
        return NULL;
    }
    static char why[48];
    snprintf(why, sizeof(why), "proof refused: %s", v >= 0 && v < 6 ? verdict_text[v] : "?");
    return why;
}

// Connected as central: link settings, discovery, START, the stream, the final ACK. NULL on success.
static const char *tx_session(const uint8_t *img)
{
    uint16_t c = tx.conn;
    struct ble_gap_conn_desc d;
    EventBits_t b;
    char miss[64] = "";
    int rc;
    if (!tune.seq) {                                  // rc.1's order: everything at once, then one wait
        rc = ble_hs_hci_util_set_data_len(c, 251, 2120);
        if (rc) ESP_LOGW(TAG, "set_data_len rc=%d", rc);
        ble_att_set_preferred_mtu(512);
        rc = ble_gattc_exchange_mtu(c, mtu_cb, NULL);
        if (rc) ESP_LOGW(TAG, "exchange_mtu rc=%d", rc);
        rc = ble_gap_set_prefered_le_phy(c, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
        if (rc) ESP_LOGW(TAG, "set_prefered_le_phy rc=%d", rc);
        b = wait_bits(B_MTU | B_PHY | B_DLE, true, 2000);
        if (b & B_DISC) return "link lost";
        if (!(b & B_DLE)) strlcat(miss, " no-dle-event", sizeof(miss));
        if (!(b & B_PHY)) strlcat(miss, " no-phy-event", sizeof(miss));
        if (!(b & B_MTU)) strlcat(miss, " no-mtu-event", sizeof(miss));
    }
    // One Link Layer procedure at a time (two at once collide and one of them is refused): data length first
    if (!tune.seq) rc = 0;
    else if ((rc = ble_hs_hci_util_set_data_len(c, 251, 2120)) != 0) ESP_LOGW(TAG, "set_data_len rc=%d", rc);
    else if (!((b = wait_bits(B_DLE, false, 1500)) & B_DLE)) { if (b & B_DISC) return "link lost"; strlcat(miss, " no-dle-event", sizeof(miss)); }
    ESP_LOGI(TAG, "link dle tx=%u rx=%u tx_time=%u us", tx.dle, tx.dle_rx, tx.dle_time);
    if (!tune.seq) rc = 0;
    else if ((rc = ble_gap_set_prefered_le_phy(c, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0)) != 0) ESP_LOGW(TAG, "set_prefered_le_phy rc=%d", rc);
    else if (!((b = wait_bits(B_PHY, false, 1500)) & B_PHY)) { if (b & B_DISC) return "link lost"; strlcat(miss, " no-phy-event", sizeof(miss)); }
    uint8_t txp = 0, rxp = 0;
    ble_gap_read_le_phy(c, &txp, &rxp);
    ESP_LOGI(TAG, "link phy tx=%s rx=%s (update status %d, event tx=%s rx=%s)", phy_name(txp), phy_name(rxp),
             tx.phy_status, phy_name(tx.phy), phy_name(tx.phy_rx));
    if (ble_gap_conn_find(c, &d) == 0 && (d.conn_itvl != tune.itvl || d.conn_latency != 0)) {
        struct ble_gap_upd_params up = { .itvl_min = tune.itvl, .itvl_max = tune.itvl, .latency = 0,
                                         .supervision_timeout = 600, .min_ce_len = tune.min_ce, .max_ce_len = tune.max_ce };
        rc = ble_gap_update_params(c, &up);
        if (rc) ESP_LOGW(TAG, "update_params rc=%d", rc);
        else if (!((b = wait_bits(B_UPD, false, 2000)) & B_UPD)) { if (b & B_DISC) return "link lost"; strlcat(miss, " no-update-event", sizeof(miss)); }
    }
    bool have_d = ble_gap_conn_find(c, &d) == 0;
    int itvl = have_d ? d.conn_itvl : 0;
    ESP_LOGI(TAG, "link itvl=%d.%02d ms latency=%u timeout=%u ms (update status %d, ce %u-%u)", itvl * 125 / 100,
             itvl * 125 % 100, have_d ? d.conn_latency : 0, have_d ? d.supervision_timeout * 10 : 0, tx.upd_status,
             tune.min_ce, tune.max_ce);
    if (tune.seq) {
        ble_att_set_preferred_mtu(512);
        rc = ble_gattc_exchange_mtu(c, mtu_cb, NULL);
        if (rc) ESP_LOGW(TAG, "exchange_mtu rc=%d", rc);
        else if (!((b = wait_bits(B_MTU, false, 2000)) & B_MTU)) { if (b & B_DISC) return "link lost"; strlcat(miss, " no-mtu-event", sizeof(miss)); }
    }
    uint16_t mtu = ble_att_mtu(c);
    ESP_LOGI(TAG, "link mtu=%u phy=%s dle=%u itvl=%d.%02d ms window=%u scan=%u ms adv=%u seq=%u%s", mtu, phy_name(txp), tx.dle,
             itvl * 125 / 100, itvl * 125 % 100, tune.window, tune.scan, tune.adv, tune.seq, miss);
    if (mtu < 64) return "MTU too small";

    tx.svc_start = tx.svc_end = tx.h_ctrl = tx.h_data = tx.h_ack = 0;
    xEventGroupClearBits(eg, B_GATT);
    if (ble_gattc_disc_svc_by_uuid(c, &svc_uuid.u, svc_cb, NULL)) return "service discovery";
    if (!(wait_bits(B_GATT, false, 5000) & B_GATT) || tx.gatt_status) return "no firmware service";
    xEventGroupClearBits(eg, B_GATT);
    if (ble_gattc_disc_all_chrs(c, tx.svc_start, tx.svc_end, chr_cb, NULL)) return "characteristic discovery";
    if (!(wait_bits(B_GATT, false, 5000) & B_GATT) || tx.gatt_status) return "characteristics missing";
    char a[18];
    if (ble_gap_conn_find(c, &d) == 0) fmt_addr(a, sizeof(a), d.peer_id_addr.val);
    else strlcpy(a, "?", sizeof(a));
    const char *pw = tx_prove(c, a);
    if (pw) return pw;

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
    if (tx.probed) {                                  // fwtune probe: tell the receiver, which backs off 30 s
        uint8_t ab = CTRL_ABORT;
        xEventGroupClearBits(eg, B_GATT);
        if (ble_gattc_write_flat(c, tx.h_ctrl, &ab, 1, write_cb, NULL) == 0) wait_bits(B_GATT, false, 1000);
        return PROBE_DONE;
    }
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
    struct ble_gap_conn_params cp = { .scan_itvl = 0x30, .scan_window = 0x30, .itvl_min = tune.itvl, .itvl_max = tune.itvl,
                                      .latency = 0, .supervision_timeout = 600, .min_ce_len = tune.min_ce, .max_ce_len = tune.max_ce };
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
    if (!(b & B_CONN)) {
        mesh_scan_pause(false);
        if (!(b & B_CONN_FAIL)) ble_gap_conn_cancel();
        ESP_LOGW(TAG, "failed: no connection to %s (status %d)", a, b & B_CONN_FAIL ? tx.conn_status : -1);
        esp_partition_munmap(mh);
        return;
    }
    ESP_LOGI(TAG, "serving %s %s %lu bytes", a, esp_app_get_description()->version, (unsigned long)run_len);
    // The flood goes on during the transfer: the scan with the tuned window (fwtune scan=, 100 = as always, 0 = off)
    mesh_set_scan_window((uint16_t)tune.scan * 8 / 5);
    if (tune.scan) mesh_scan_pause(false);
    if (!tune.adv) mesh_adv_pause(true);
    int64_t t0 = esp_timer_get_time();
    const char *why = tx_session(img);
    int64_t us = esp_timer_get_time() - t0;
    mesh_stats_t st;
    mesh_get_stats(&st);
    char s[12], r[12], ss[12], rs[12];
    uint32_t nsec = (run_len + SECTOR - 1) / SECTOR;
    uint32_t sent = !why ? run_len : tx.acked < 0 ? 0 : tx.acked + 1 >= (int32_t)nsec ? run_len : (uint32_t)(tx.acked + 1) * SECTOR;
    fmt_rate(s, sizeof(s), r, sizeof(r), run_len, us);
    fmt_rate(ss, sizeof(ss), rs, sizeof(rs), sent, esp_timer_get_time() - tx.t_stream_us);
    if (!why) {
        ESP_LOGI(TAG, "served %s in %s s (%s KB/s; stream %s KB/s, %lu resent, %lu ENOMEM %lu ms, window wait %lu ms; "
                 "flood %lu reports/s)", a, s, r, rs, (unsigned long)tx.resent, (unsigned long)tx.enomem,
                 (unsigned long)tx.enomem_ms, (unsigned long)tx.wait_ms, (unsigned long)st.colormesh_s);
    } else if (why == PROBE_DONE) {
        ESP_LOGI(TAG, "probe %s: %lu KB in %s s (%s KB/s, %lu resent, %lu ENOMEM %lu ms, window wait %lu ms; "
                 "flood %lu reports/s)", a, (unsigned long)(sent / 1024), ss, rs, (unsigned long)tx.resent,
                 (unsigned long)tx.enomem, (unsigned long)tx.enomem_ms, (unsigned long)tx.wait_ms, (unsigned long)st.colormesh_s);
    } else {
        ESP_LOGW(TAG, "failed: %s (serving %s, %lu KB sent)", why, a, (unsigned long)(sent / 1024));
    }
    if (!(xEventGroupGetBits(eg) & B_DISC)) {
        ble_gap_terminate(tx.conn, BLE_ERR_REM_USER_CONN_TERM);
        xEventGroupWaitBits(eg, B_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
    }
    tx.conn = BLE_HS_CONN_HANDLE_NONE;
    esp_partition_munmap(mh);
    mesh_set_scan_window(0);                          // the flood's continuous scan and advertising again
    mesh_scan_pause(false);
    if (!tune.adv) mesh_adv_pause(false);
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
    bool admin = config_get()->admin && config_get()->has_admin_key;
#ifndef MESH_PEER_HAS_TARGET
    uint8_t own[6] = { 0 };
    unsigned v[6];
    if (sscanf(mesh_own_addr(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return;
    for (int i = 0; i < 6; i++) own[5 - i] = (uint8_t)v[i];
#endif
    for (int i = 0; i < n; i++) {
        const mesh_peer_t *p = &peers[i];
        if (!p->dev && !admin) continue;              // a peer out of dev mode wants a signed image: the admin's
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
// first-sector check, esp_ota_begin/write/end) into the other slot. The boot slot is not changed. With an admin key
// stored, the signed path too: a nonce, this board's proof (signed with its private key, checked with its own public
// key: any version counts as newer here), the START check, the streamed digest at the end. mode 1 ("baddigest")
// changes the signed digest after the START check, so only the final digest compare can catch it ("fw: failed:
// digest mismatch"); mode 2 ("badsig") flips a signature bit ("fw: refused self: bad signature").
enum { LOOP_PLAIN, LOOP_BAD_DIGEST, LOOP_BAD_SIG };
static void do_loop(uint32_t mode)
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
    rx_auth_reset();
    last_fail[0] = 0;
    const char *auth = "none";
    uint32_t sign_ms = 0, verify_ms = 0;
    const app_cfg_t *c = config_get();
    if (c->has_admin_key && run_hashed) {
        static const uint8_t any_ver[4] = { 0, 0, 0, 0 };   // the own version: newer than 0.0.0, as fwpull skips it
        uint8_t n[FWA_NONCE_LEN];
        esp_fill_random(n, sizeof(n));
        fwa_nonce_issue(&rx.nonce, n, now_ms());
        fwa_proof_t p;
        int rc = proof_make(&p, n, &sign_ms);
        fwa_zero(n, sizeof(n));
        const char *jw = "signing failed";
        uint8_t v = FWA_V_BAD_PROOF;
        if (!rc) {
            if (mode == LOOP_BAD_SIG) p.sig[40] ^= 0x01;
            v = proof_judge(&p, any_ver, c->admin_pub, &jw, &verify_ms);
        }
        if (v != FWA_V_OK || !fwa_start_matches(&p, run_len, run_digest)) {
            ESP_LOGW(TAG, "refused self: %s", v != FWA_V_OK ? jw : "START does not match the signed image");
            ESP_LOGW("test", "error fwloop: proof %s", v != FWA_V_OK ? jw : "START mismatch");
            rx_auth_reset();
            esp_partition_munmap(mh);
            rx.loop = tx.loop = false;
            rx.state = R_IDLE;
            progress = -1;
            return;
        }
        rx.proof = p;
        rx.proof_ok = true;
        if (mode == LOOP_BAD_DIGEST) rx.proof.digest[31] ^= 0x01;   // past the START check: only the end can see it
        auth = "signed";
    }
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
    rx_auth_reset();
    char s[12], r[12];
    fmt_rate(s, sizeof(s), r, sizeof(r), run_len, us);
    if (why) ESP_LOGW("test", "error fwloop: %s%s%s%s", why, last_fail[0] ? " (" : "", last_fail, last_fail[0] ? ")" : "");
    else
        ESP_LOGI("test", "fwloop ok part=%s len=%lu sectors=%lu crc_errors=%lu resent=%lu s=%s kbps=%s write_max_ms=%lu "
                 "auth=%s sign_ms=%lu verify_ms=%lu stack_free=%u", rx.part->label, (unsigned long)run_len,
                 (unsigned long)rx.nsec, (unsigned long)rx.nacks, (unsigned long)tx.resent, s, r,
                 (unsigned long)(rx.write_us_max / 1000), auth, (unsigned long)sign_ms, (unsigned long)verify_ms,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

// ======================================================================================================
// Admin and trust (console "admin ...", "trust ..."; fw task: the ECDSA needs this task's stack)
// ======================================================================================================

EXT_RAM_BSS_ATTR static uint8_t trust_in[FWA_PUB_LEN];   // console -> fw task
static volatile bool trust_busy;

static bool admin_make_key(void)
{
    uint8_t priv[FWA_PRIV_LEN], pub[FWA_PUB_LEN];
    char old[FWA_SHORT_LEN + 1] = "";
    const app_cfg_t *c = config_get();
    if (c->has_admin_key) short_of(c->admin_pub, old);
    int64_t t0 = esp_timer_get_time();
    int rc = ec_keygen(priv, pub);
    bool ok = !rc && config_set_admin_key(priv, pub);
    fwa_zero(priv, sizeof(priv));
    if (!ok) { ESP_LOGW("test", "error admin: no key pair (%s -0x%04x)", rc ? "keygen" : "nvs", rc ? -rc : 0); return false; }
    char k[FWA_SHORT_LEN + 1];
    short_of(pub, k);
    ESP_LOGI(TAG, "admin key new %s (%lu ms)%s%s%s", k, (unsigned long)ms_since(t0), old[0] ? ", replaces " : "", old,
             old[0] ? ": boards that trusted it must trust the new one" : "");
    return true;
}

static void do_selftest(void)
{
    const app_cfg_t *c = config_get();
    if (!c->has_admin_key) { ESP_LOGW("test", "error admin selftest: no key (admin on makes one)"); return; }
    uint8_t m[64], h[32], sig[FWA_SIG_LEN];
    esp_fill_random(m, sizeof(m));
    mbedtls_sha256(m, sizeof(m), h, 0);
    int64_t t0 = esp_timer_get_time();
    int src = ec_sign(h, sig);
    uint32_t sms = ms_since(t0);
    t0 = esp_timer_get_time();
    int vrc = src ? -1 : ec_verify(c->admin_pub, h, sig);
    uint32_t vms = ms_since(t0);
    h[0] ^= 0x01;
    int trc = src ? -1 : ec_verify(c->admin_pub, h, sig);   // a changed message must not verify
    if (src || vrc || !trc) {
        ESP_LOGW("test", "error admin selftest: sign -0x%04x verify -0x%04x tampered %s", src < 0 ? -src : src,
                 vrc < 0 ? -vrc : vrc, trc ? "refused" : "ACCEPTED");
        return;
    }
    ESP_LOGI("test", "ok admin selftest %lu sign_ms=%lu verify_ms=%lu tamper=refused stack_free=%u internal_min=%u",
             (unsigned long)(sms + vms), (unsigned long)sms, (unsigned long)vms,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

static void do_admin(uint32_t op)
{
    const app_cfg_t *c = config_get();
    char k[FWA_SHORT_LEN + 1];
    switch (op) {
    case MESH_FW_ADMIN_ON:
        if (!c->has_admin_key && !admin_make_key()) return;
        config_set_admin(true);
        mesh_set_admin(true);                         // the advert's MP_F_ADMIN
        short_of(c->admin_pub, k);
        ESP_LOGI(TAG, "admin on (key %s)", k);
        ESP_LOGI("test", "ok admin on %s", k);
        break;
    case MESH_FW_ADMIN_OFF:
        config_set_admin(false);
        mesh_set_admin(false);
        ESP_LOGI(TAG, "admin off");
        ESP_LOGI("test", "ok admin off");
        break;
    case MESH_FW_ADMIN_NEW:
        if (!admin_make_key()) return;
        short_of(c->admin_pub, k);
        if (c->admin) ESP_LOGI(TAG, "admin on (key %s)", k);
        ESP_LOGI("test", "ok admin new %s", k);
        break;
    case MESH_FW_ADMIN_SELFTEST:
        do_selftest();
        break;
    }
}

static void do_trust(uint32_t set)
{
    char k[FWA_SHORT_LEN + 1];
    if (!set) {
        config_clear_trusted();
        ESP_LOGI(TAG, "trusted admin none");
        ESP_LOGI("test", "ok trust none");
        return;
    }
    uint8_t pub[FWA_PUB_LEN];
    memcpy(pub, trust_in, sizeof(pub));
    trust_busy = false;
    if (!ec_pub_valid(pub)) { ESP_LOGW("test", "error trust: not a P-256 public key"); return; }
    if (!config_set_trusted(pub)) { ESP_LOGW("test", "error trust: not saved"); return; }
    short_of(pub, k);
    ESP_LOGI(TAG, "trusted admin %s", k);
    ESP_LOGI("test", "ok trust %s\u2026", k);
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
    case E_RXERR:
        if (rx.state == R_IDLE) break;
        if (a == RXE_NO_PROOF || a == RXE_UNSIGNED_START) rx_refuse(rxe_text[a]);   // "fw: refused <addr>: no proof"
        else rx_fail("%s", rxe_text[a < RXE_N ? a : 0]);
        break;
    case E_CHAL: rx_on_challenge_request(); break;
    case E_PROOF: rx_on_proof(); break;
    case E_ADMIN: do_admin(a); break;
    case E_TRUST: do_trust(a); break;
    case E_INFO: do_info(); break;
    case E_LOOP: do_loop(a); break;
    case E_LINK: rx_on_link(a); break;
    case E_TUNE:
        ESP_LOGI("test", "fwtune window=%u probe=%u scan=%u adv=%u rxscan=%u itvl=%u mince=%u maxce=%u seq=%u", tune.window,
                 tune.probe_kb, tune.scan, tune.adv, tune.rxscan, tune.itvl, tune.min_ce, tune.max_ce, tune.seq);
        break;
    case E_CONFIRM: do_confirm(); break;
    }
}

static void fw_task(void *arg)
{
    uint32_t next_poll = now_ms() + POLL_MS;
    for (;;) {
        uint32_t e;
        if (rx.state == R_LINKED) {                   // short waits before START: the challenge and the proof
            if (xQueueReceive(secq, &e, pdMS_TO_TICKS(rx.active ? 200 : 20)) == pdTRUE) {
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
static void cmd_fwloop(int argc, char **argv)
{
    uint32_t mode = LOOP_PLAIN;
    if (argc == 2 && !strcmp(argv[1], "baddigest")) mode = LOOP_BAD_DIGEST;
    else if (argc == 2 && !strcmp(argv[1], "badsig")) mode = LOOP_BAD_SIG;
    else if (argc != 1) { ESP_LOGW("test", "error fwloop: expected nothing, baddigest or badsig"); return; }
    post(evq, EV(E_LOOP, mode));
}

// fwtune [key=value ...]: window=1..8 probe=KB scan=0..100 adv=0|1 rxscan=0|1 itvl=6..3200 mince=N maxce=N (RAM only)
static void cmd_fwtune(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { ESP_LOGW("test", "error fwtune: expected key=value, got %s", argv[i]); return; }
        *eq = 0;
        long v = strtol(eq + 1, NULL, 10);
        const char *k = argv[i];
        if (!strcmp(k, "window") && v >= 1 && v <= RING) tune.window = (uint8_t)v;
        else if (!strcmp(k, "probe") && v >= 0 && v <= 65535) tune.probe_kb = (uint16_t)v;
        else if (!strcmp(k, "scan") && v >= 0 && v <= 100) tune.scan = (uint8_t)v;
        else if (!strcmp(k, "adv") && (v == 0 || v == 1)) tune.adv = (uint8_t)v;
        else if (!strcmp(k, "rxscan") && (v == 0 || v == 1)) tune.rxscan = (uint8_t)v;
        else if (!strcmp(k, "itvl") && v >= 6 && v <= 3200) tune.itvl = (uint16_t)v;
        else if (!strcmp(k, "mince") && v >= 0 && v <= 0xFFFF) tune.min_ce = (uint16_t)v;
        else if (!strcmp(k, "maxce") && v >= 0 && v <= 0xFFFF) tune.max_ce = (uint16_t)v;
        else if (!strcmp(k, "seq") && (v == 0 || v == 1)) tune.seq = (uint8_t)v;
        else { ESP_LOGW("test", "error fwtune: bad %s=%s", k, eq + 1); return; }
    }
    post(evq, EV(E_TUNE, 0));
}
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
    testcon_register("fwloop", "fwloop [baddigest|badsig]", cmd_fwloop);
    testcon_register("fwtune", "fwtune [window=N probe=KB scan=MS adv=0|1 rxscan=0|1 itvl=N mince=N maxce=N seq=0|1]", cmd_fwtune);
    if (xTaskCreatePinnedToCore(fw_task, "fw", FW_STACK, NULL, 4, &fw_task_h, 0) != pdPASS) {
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

void mesh_fw_admin(int op)
{
    post(evq, EV(E_ADMIN, op));
}

bool mesh_fw_trust(const uint8_t pub[64])
{
    if (!pub) { post(evq, EV(E_TRUST, 0)); return true; }
    if (trust_busy) return false;
    memcpy(trust_in, pub, sizeof(trust_in));
    trust_busy = true;
    post(evq, EV(E_TRUST, 1));
    return true;
}

void mesh_fw_set_target(uint8_t origin)
{
    forced = origin;
    post(evq, EV(E_TARGET, origin));
}

int mesh_fw_progress(void) { return progress; }
