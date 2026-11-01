// The mesh (see mesh.h): a NimBLE advertising flood (docs/RESEARCH-BLE.md). Every board advertises its own state in
// one 30-byte manufacturer-data AD (mesh_proto.h) every 20 ms, non-connectable, and scans continuously. The GAP
// callback only copies each ColorMesh report into a queue (microseconds, no logging); the mesh task owns the peer
// table, the expiry tick, the mix, the "mesh:" log lines and every advertising change, so no touch or LVGL handler
// ever calls NimBLE. The payload, peer table and mixing are mesh_proto.c; the firmware pull is mesh_fw.c.
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG   // compiles the per-report "ble: other" lines in; off until "blelog debug"
#include "mesh.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "testcon.h"
#include "config.h"
#include "mesh_proto.h"
#include "mesh_fw.h"
#include "rps.h"

static const char *TAG = "mesh";
static const char *TAG_BLE = "ble";

#define MESH_Q_LEN      32
#define OTHER_Q_LEN     32
#define ADV_ITVL_FAST   0x20            // 20 ms, the NimBLE floor without CONFIG_BT_NIMBLE_HIGH_DUTY_ADV_ITVL
#define ADV_ITVL_SLOW   0xA0            // 100 ms: the fallback if the controller refuses 20 ms (risk 1)
#define SCAN_ITVL       0xA0            // 100 ms window = interval: continuous
#define SCAN_WINDOW     0xA0
#define TICK_MS         50              // expiry and mix tick
#define RETRY_MS        1000            // a refused adv/scan start is retried this often

// One report from the GAP callback to the mesh task. len 0 = a wake-up (state change, radio event), no data.
typedef struct {
    uint8_t addr[6];
    int8_t rssi;
    uint8_t len;
    uint8_t mfg[MP_MFG_LEN];
} report_t;

// A non-ColorMesh report, only while "ble" logs at DEBUG (one line each, from the mesh task)
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type, evt, len;
    int8_t rssi;
} other_t;

// ---- state ----

static volatile bool holding, dev_mode, admin, connectable, adv_paused, scan_paused, synced;
static volatile uint8_t own_r, own_g, own_b, target = MP_TARGET_NONE;
static volatile uint32_t own_b2;        // the waves' colour B: 0x01rrggbb when chosen, 0 = automatic (one word: no torn read)
static uint8_t own_addr_type;
static uint8_t own_raw[6];
static volatile uint8_t own_origin;
static char own_addr[18];
static uint8_t own_ver[4];

static mp_table_t table;                // under table_lock
static SemaphoreHandle_t table_lock;
static SemaphoreHandle_t radio_lock;    // every advertising/scan start, stop and data change
static QueueHandle_t q, q_other;
static volatile bool refresh_req;
static volatile uint32_t mix_packed;    // 0x00rrggbb of the last mix
static volatile mesh_gap_hook_t gap_hook;
static volatile mesh_clip_cb_t clip_cb;     // clip.c: a peer announced a new clip
static volatile mesh_ring_cb_t ring_cb;     // clip.c: a peer calls
static volatile mesh_ring_cb_t answer_cb;   // clip.c: a peer answered this board's call
static volatile uint8_t own_answer_to;       // byte 28 with MP_EFFECT_ANSWER: the caller answered

// Voice clips: the own bytes 26-27 and MP_F_CLIP; per peer address the last clip id handed to clip_cb
static volatile uint8_t own_clip_id, own_clip_ds;
static volatile bool own_clip_on;
static struct {
    uint8_t addr[6], id;
    bool used;
} clip_seen[MP_MAX_PEERS];                  // mesh task only

// Waves (mesh_start_wave): the own effect bytes in the advert, the waves running here, the peers' last effect seen
static volatile uint8_t own_effect, own_effect_seq, effect_kind_req;
static volatile bool effect_req;
static int64_t effect_clear_us;         // mesh task: when the effect bytes go back to 0 (0 = none pending)
static wave_t waves[WAVE_MAX];          // under table_lock
static bool waves_any;                  // under table_lock: a wave started and "mesh: wave end" is still to come
static struct {
    uint8_t addr[6], seq;
    bool used, active;                  // active: an effect != 0 heard with this seq (a new seq or a new effect after
                                        // an effect 0 starts a wave)
    uint32_t last_ms;
} eff[MP_MAX_PEERS];                    // mesh task only
static const char *const wave_names[] = { "none", "centre", "left", "right", "top", "bottom" };

// Rock paper scissors (mesh_game_*): the round, the result frozen when the collect phase ends
static rps_round_t game;                // under table_lock
static uint8_t game_seq_own;            // under table_lock: the last round seq this board started
static bool game_frozen;                // under table_lock: game_res holds this round's result
static mesh_game_t game_res;            // under table_lock
static uint8_t game_adv[4];             // mesh task only: the game bytes last advertised (seq, starter, left, pick)

static uint8_t adv_buf[MP_AD_SIZE];     // under radio_lock
static uint8_t seq;                     // mesh task only
static volatile bool adv_on, scan_on, adv_fallback;
static volatile uint16_t adv_itvl;
static int64_t adv_retry_us, scan_retry_us;
static uint16_t scan_itvl = SCAN_ITVL, scan_window = SCAN_WINDOW;

// counters: written by the host task (callback) and the mesh task, read anywhere
static volatile uint32_t n_reports, n_cm, n_drops;
static volatile uint32_t cb_us_max, cb_us_max_all, cb_us_max_last;
static volatile uint32_t q_hwm, q_hwm_all, q_hwm_last;
static volatile uint32_t rate_rep_last, rate_cm_last;
static volatile bool dbg_other;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void wake(void)
{
    static const report_t w = { .len = 0 };
    if (q) xQueueSendToFront(q, &w, 0);   // a full queue means the task is draining anyway: it sees the flags
}

static void fmt_addr(char *out, size_t n, const uint8_t a[6])
{
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

// vX.Y.Z plus the kind byte: 0..99 -> -N, 100..199 -> -rcN, 200 -> nothing, 201+ -> +N
static void fmt_ver(char *out, size_t n, const uint8_t v[4])
{
    int k = n > 0 ? snprintf(out, n, "v%u.%u.%u", v[0], v[1], v[2]) : 0;
    if (k < 0 || (size_t)k >= n) return;
    if (v[3] < MP_VER_RC) snprintf(out + k, n - k, "-%u", v[3]);
    else if (v[3] < MP_VER_RELEASE) snprintf(out + k, n - k, "-rc%u", v[3] - MP_VER_RC);
    else if (v[3] > MP_VER_RELEASE) snprintf(out + k, n - k, "+%u", v[3] - MP_VER_POST);
}

// Servable while the running image is valid (or has no otadata state, as after a cable flash); pending while it
// still has to confirm itself after an update
static uint8_t ota_flags(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return MP_F_SERVABLE;
    if (st == ESP_OTA_IMG_VALID || st == ESP_OTA_IMG_UNDEFINED) return MP_F_SERVABLE;
    if (st == ESP_OTA_IMG_PENDING_VERIFY) return MP_F_PENDING;
    return 0;
}

// ---- GAP events (NimBLE host task) ----

// One advertising report (legacy or extended scan): copy a ColorMesh payload into the queue
static void on_report(const ble_addr_t *addr, int8_t rssi, uint8_t evt, const uint8_t *data, uint8_t len)
{
    int64_t t0 = esp_timer_get_time();
    n_reports++;
    const struct ble_hs_adv_field *f;
    if (ble_hs_adv_find_field(BLE_HS_ADV_TYPE_MFG_DATA, data, len, &f) == 0 &&
        f->length - 1 == MP_MFG_LEN && f->value[2] == MP_PROTO) {
        report_t r;
        memcpy(r.addr, addr->val, 6);
        r.rssi = rssi;
        r.len = MP_MFG_LEN;
        memcpy(r.mfg, f->value, MP_MFG_LEN);       // desc->data points into the HCI buffer: copy now
        n_cm++;
        if (xQueueSend(q, &r, 0) != pdTRUE) n_drops++;
    } else if (dbg_other) {
        other_t o = { .addr_type = addr->type, .evt = evt, .len = len, .rssi = rssi };
        memcpy(o.addr, addr->val, 6);
        xQueueSend(q_other, &o, 0);
    }
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > cb_us_max) cb_us_max = us;
}

static int gap_event(struct ble_gap_event *e, void *arg)
{
    if (e->type == BLE_GAP_EVENT_DISC) {             // the hot path: copy and return, nothing logged
        on_report(&e->disc.addr, e->disc.rssi, e->disc.event_type, e->disc.data, e->disc.length_data);
        return 0;
    }
#if MYNEWT_VAL(BLE_EXT_ADV)
    if (e->type == BLE_GAP_EVENT_EXT_DISC) {         // with extended advertising on, every report comes this way
        const struct ble_gap_ext_disc_desc *d = &e->ext_disc;
        if (d->props & BLE_HCI_ADV_LEGACY_MASK)      // a clip's own set is not legacy: the controller syncs to it
            on_report(&d->addr, d->rssi, d->legacy_event_type, d->data, d->length_data);
        return 0;
    }
#endif
    mesh_gap_hook_t hook = gap_hook;
    if (hook) hook(e);
    switch (e->type) {
    case BLE_GAP_EVENT_DISC_COMPLETE:               // the scan ended (it shouldn't, BLE_HS_FOREVER): restart it
        scan_on = false;
        wake();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:                // advertising ended (a central connected, or it was stopped)
    case BLE_GAP_EVENT_CONNECT:                     // a central connected to our connectable advertisement
    case BLE_GAP_EVENT_DISCONNECT:
        wake();                                     // the mesh task checks adv_active() and restarts
        break;
    default:
        break;
    }
    return 0;
}

// ---- radio (radio_lock held) ----

#if MYNEWT_VAL(BLE_EXT_ADV)
// The flood is extended advertising instance 0 with legacy PDUs: on air the same ADV_NONCONN_IND / ADV_IND as before,
// so boards on older firmware still hear it. Instance 1 is the clip's periodic set (mesh_clip_adv_*).
static int adv_data_locked(void)
{
    struct os_mbuf *m = os_msys_get_pkthdr(MP_AD_SIZE, 0);
    if (!m) return BLE_HS_ENOMEM;
    int rc = os_mbuf_append(m, adv_buf, MP_AD_SIZE);
    if (rc) { os_mbuf_free_chain(m); return rc; }
    return ble_gap_ext_adv_set_data(MESH_ADV_FLOOD, m);   // takes the mbuf, even on failure
}

static int adv_begin_locked(uint16_t itvl)
{
    struct ble_gap_ext_adv_params p = { 0 };
    p.legacy_pdu = 1;
    p.connectable = p.scannable = connectable;    // legacy connectable = ADV_IND (connectable + scannable)
    p.itvl_min = p.itvl_max = itvl;
    p.own_addr_type = own_addr_type;
    p.primary_phy = p.secondary_phy = BLE_HCI_LE_PHY_1M;
    p.sid = MESH_ADV_FLOOD;
    int rc = ble_gap_ext_adv_configure(MESH_ADV_FLOOD, &p, NULL, gap_event, NULL);
    if (rc) return rc;
    if ((rc = adv_data_locked()) != 0) ESP_LOGW(TAG_BLE, "adv_set_data rc=%d", rc);
    return ble_gap_ext_adv_start(MESH_ADV_FLOOD, 0, 0);
}
static bool adv_active(void) { return ble_gap_ext_adv_active(MESH_ADV_FLOOD); }
#else
static int adv_data_locked(void) { return ble_gap_adv_set_data(adv_buf, MP_AD_SIZE); }

static int adv_begin_locked(uint16_t itvl)
{
    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
    p.disc_mode = BLE_GAP_DISC_MODE_NON;
    int rc = ble_gap_adv_set_data(adv_buf, MP_AD_SIZE);
    if (rc) ESP_LOGW(TAG_BLE, "adv_set_data rc=%d", rc);
    p.itvl_min = p.itvl_max = itvl;
    return ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}
static bool adv_active(void) { return ble_gap_adv_active(); }
#endif

static void adv_start_locked(void)
{
    uint16_t itvl = adv_fallback ? ADV_ITVL_SLOW : ADV_ITVL_FAST;
    int rc = adv_begin_locked(itvl);
    if (rc && rc != BLE_HS_EALREADY && !adv_fallback) {
        ESP_LOGW(TAG_BLE, "adv_start rc=%d", rc);
        ESP_LOGW(TAG_BLE, "20 ms advertising refused: falling back to 100 ms (0xA0)");
        adv_fallback = true;
        itvl = ADV_ITVL_SLOW;
        rc = adv_begin_locked(itvl);
    }
    if (rc && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG_BLE, "adv_start rc=%d (retry in %d ms)", rc, RETRY_MS);
        adv_retry_us = esp_timer_get_time() + RETRY_MS * 1000LL;
        return;
    }
    adv_on = true;
    adv_itvl = itvl;
    ESP_LOGI(TAG_BLE, "advertising %u ms%s", itvl * 5u / 8u, connectable ? " connectable" : "");
}

static void adv_stop_locked(void)
{
    if (adv_active()) {
#if MYNEWT_VAL(BLE_EXT_ADV)
        int rc = ble_gap_ext_adv_stop(MESH_ADV_FLOOD);
#else
        int rc = ble_gap_adv_stop();
#endif
        if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG_BLE, "adv_stop rc=%d", rc);
    }
    adv_on = false;
    adv_itvl = 0;
}

static void scan_start_locked(void)
{
    struct ble_gap_disc_params p = { .itvl = scan_itvl, .window = scan_window, .filter_policy = 0, .limited = 0,
                                     .passive = 1, .filter_duplicates = 0 };
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG_BLE, "disc rc=%d (retry in %d ms)", rc, RETRY_MS);
        scan_retry_us = esp_timer_get_time() + RETRY_MS * 1000LL;
        return;
    }
    scan_on = true;
    ESP_LOGI(TAG_BLE, "scanning %u/%u ms", scan_window * 5u / 8u, scan_itvl * 5u / 8u);
}

static void scan_stop_locked(void)
{
    if (ble_gap_disc_active()) {
        int rc = ble_gap_disc_cancel();
        if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG_BLE, "disc_cancel rc=%d", rc);
    }
    scan_on = false;
}

// Brings advertising and scanning to what the flags want (mesh task tick and wake-ups)
static void radio_check(void)
{
    if (!synced) return;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    if (adv_on && !adv_active()) { adv_on = false; adv_itvl = 0; }
    if (!adv_paused && !adv_on && now >= adv_retry_us) adv_start_locked();
    if (scan_on && !ble_gap_disc_active()) scan_on = false;
    if (!scan_paused && !scan_on && now >= scan_retry_us) scan_start_locked();
    xSemaphoreGive(radio_lock);
}

// ---- mesh task ----

// This board's state changed (or sync, or mesh_refresh): bump seq, update self in the table, re-encode, and
// update the advertisement in place (no stop/start)
static void do_refresh(void)
{
    mp_state_t s = { 0 };
    s.origin = own_origin;
    s.seq = ++seq;
    s.ttl = 0;
    s.flags = (dev_mode ? MP_F_DEV : 0) | (admin ? MP_F_ADMIN : 0) | (own_clip_on ? MP_F_CLIP : 0) | ota_flags();
    s.clip_id = own_clip_id;
    s.clip_ds = own_clip_ds;
    s.answer_to = own_effect == MP_EFFECT_ANSWER ? own_answer_to : 0;
    s.r = own_r;
    s.g = own_g;
    s.b = own_b;
    memcpy(s.ver, own_ver, 4);
    s.target = target;
    s.effect = own_effect;
    s.effect_seq = own_effect_seq;
    uint32_t b2 = own_b2;                            // bytes 19-21: 0, 0, 0 = automatic
    if (b2) {
        s.b2r = (uint8_t)(b2 >> 16);
        s.b2g = (uint8_t)(b2 >> 8);
        s.b2b = (uint8_t)b2;
        if (!(s.b2r | s.b2g | s.b2b)) s.b2r = s.b2g = s.b2b = 1;   // a chosen black: #010101 (0 means automatic)
    }
    bool h = holding;
    uint8_t buf[MP_AD_SIZE];
    xSemaphoreTake(table_lock, portMAX_DELAY);
    rps_advert(&game, now_ms(), &s.game_starter, &s.game_seq, &s.game_left, &s.game_pick);
    game_adv[0] = s.game_seq;
    game_adv[1] = s.game_starter;
    game_adv[2] = s.game_left;
    game_adv[3] = s.game_pick;
    mp_table_set_self(&table, &s, h);
    s = table.self;                                  // with the holding bit
    xSemaphoreGive(table_lock);
    mp_encode(&s, buf, sizeof(buf));
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    memcpy(adv_buf, buf, sizeof(adv_buf));
    if (adv_on) {
        int rc = adv_data_locked();
        if (rc) ESP_LOGW(TAG_BLE, "adv_set_data rc=%d", rc);
    }
    xSemaphoreGive(radio_lock);
}

static mp_peer_t snap[MP_MAX_PEERS], shadow[MP_MAX_PEERS];   // mesh task only

static bool peer_differs(const mp_state_t *a, const mp_state_t *b)
{
    return a->r != b->r || a->g != b->g || a->b != b->b || a->flags != b->flags || a->target != b->target ||
           memcmp(a->ver, b->ver, 4) != 0;
}

// The table changed: log new and changed peers ("mesh: peer ..."), the ones that left, and a changed mix
static void after_change(void)
{
    uint8_t r, g, b;
    xSemaphoreTake(table_lock, portMAX_DELAY);
    memcpy(snap, table.peers, sizeof(snap));
    mp_mix(&table, &r, &g, &b);
    int n = mp_holders(&table);
    xSemaphoreGive(table_lock);

    char a[18], v[24];
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        if (!snap[i].used) continue;
        const mp_peer_t *old = NULL;
        for (int j = 0; j < MP_MAX_PEERS; j++)
            if (shadow[j].used && !memcmp(shadow[j].addr, snap[i].addr, 6)) { old = &shadow[j]; break; }
        if (old && !peer_differs(&old->st, &snap[i].st)) continue;
        const mp_state_t *s = &snap[i].st;
        fmt_addr(a, sizeof(a), snap[i].addr);
        fmt_ver(v, sizeof(v), s->ver);
        char extra[16] = "";
        if (s->target != MP_TARGET_NONE) snprintf(extra, sizeof(extra), " target=%02x", s->target);
        ESP_LOGI(TAG, "peer %s %s #%02x%02x%02x %s%s%s%s", a, v, s->r, s->g, s->b,
                 (s->flags & MP_F_HOLDING) ? "hold" : "idle", (s->flags & MP_F_DEV) ? " dev" : "",
                 (s->flags & MP_F_ADMIN) ? " admin" : "", extra);
    }
    for (int j = 0; j < MP_MAX_PEERS; j++) {
        if (!shadow[j].used) continue;
        bool still = false;
        for (int i = 0; i < MP_MAX_PEERS && !still; i++)
            still = snap[i].used && !memcmp(snap[i].addr, shadow[j].addr, 6);
        if (!still) {
            fmt_addr(a, sizeof(a), shadow[j].addr);
            ESP_LOGI(TAG, "peer %s gone", a);
        }
    }
    memcpy(shadow, snap, sizeof(shadow));

    static int last_n = -1;
    uint32_t packed = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (packed != mix_packed || n != last_n) {
        mix_packed = packed;
        last_n = n;
        ESP_LOGI(TAG, "mix #%06lx n=%d", (unsigned long)packed, n);
    }
}

// Over: started at or before now and no longer active (a wave started by another task just after `now` was read is
// not over: its start is after now)
static bool wave_over(const wave_t *w, uint32_t now)
{
    return w->kind == WAVE_NONE || ((int32_t)(now - w->start_ms) >= 0 && !wave_active(w, now));
}

// " B #rrggbb" for a chosen colour B (0x01rrggbb), "" for automatic: the end of a "mesh: wave" line
static const char *b2_note(char out[12], uint32_t b2)
{
    if (b2) snprintf(out, 12, " B #%06lx", (unsigned long)(b2 & 0xFFFFFF));
    else out[0] = 0;
    return out;
}

// A wave starts here now (table_lock held): a free slot, else the one that started first. b2: colour B as
// 0x01rrggbb, 0 = automatic (wave_opposite)
static void wave_add_locked(uint8_t kind, uint8_t r, uint8_t g, uint8_t b, uint32_t b2, uint32_t now)
{
    int k = -1;
    for (int i = 0; i < WAVE_MAX && k < 0; i++) if (wave_over(&waves[i], now)) k = i;
    if (k < 0) {
        k = 0;
        for (int i = 1; i < WAVE_MAX; i++) if ((int32_t)(waves[i].start_ms - waves[k].start_ms) < 0) k = i;
    }
    waves[k] = (wave_t){ .kind = kind, .r = r, .g = g, .b = b, .start_ms = now, .b2r = (uint8_t)(b2 >> 16),
                         .b2g = (uint8_t)(b2 >> 8), .b2b = (uint8_t)b2, .b2_set = b2 != 0 };
    waves_any = true;
}

// One decoded report: a peer's new effect starts its wave (receiver time, its colour and its colour B)
static void peer_effect(const uint8_t addr[6], const mp_state_t *st, uint32_t now)
{
    int k = -1, oldest = 0;
    for (int i = 0; i < MP_MAX_PEERS; i++) {
        if (eff[i].used && !memcmp(eff[i].addr, addr, 6)) { k = i; break; }
        if (!eff[i].used) oldest = i;
        else if (eff[oldest].used && (int32_t)(eff[i].last_ms - eff[oldest].last_ms) < 0) oldest = i;
    }
    if (k < 0) {
        if (st->effect == WAVE_NONE) return;          // nothing to remember
        k = oldest;
        eff[k].used = true;
        eff[k].active = false;
        memcpy(eff[k].addr, addr, 6);
    }
    if (eff[k].active && now - eff[k].last_ms > 2 * MESH_EFFECT_MS) eff[k].active = false;   // a long gap: forget it
    eff[k].last_ms = now;
    char a[18];
    if (st->effect == WAVE_NONE) {
        if (eff[k].active) {
            fmt_addr(a, sizeof(a), addr);
            ESP_LOGD(TAG_BLE, "effect %s 0 (seq %u)", a, st->effect_seq);
        }
        eff[k].active = false;
        return;
    }
    if (eff[k].active && eff[k].seq == st->effect_seq) return;
    eff[k].active = true;
    eff[k].seq = st->effect_seq;
    fmt_addr(a, sizeof(a), addr);
    ESP_LOGD(TAG_BLE, "effect %s %u (seq %u)", a, st->effect, st->effect_seq);
    if (st->effect == MP_EFFECT_ANSWER) {
        if (st->answer_to != own_origin) return;      // it answers another board's call
        ESP_LOGI(TAG, "answer from %s", a);
        mesh_ring_cb_t cb = answer_cb;
        if (cb) cb(st->origin, st->r, st->g, st->b);
        return;
    }
    if (st->effect == MP_EFFECT_RING) {
        ESP_LOGI(TAG, "ring from %s", a);
        mesh_ring_cb_t cb = ring_cb;
        if (cb) cb(st->origin, st->r, st->g, st->b);
        return;
    }
    if (st->effect > WAVE_BOTTOM) return;             // a kind this build does not know
    xSemaphoreTake(table_lock, portMAX_DELAY);
    uint32_t b2 = (st->b2r | st->b2g | st->b2b) ? 0x01000000u | (uint32_t)st->b2r << 16 | (uint32_t)st->b2g << 8 | st->b2b
                                                : 0;
    wave_add_locked(st->effect, st->r, st->g, st->b, b2, now);
    xSemaphoreGive(table_lock);
    char note[12];
    ESP_LOGI(TAG, "wave %s from %s%s", mesh_wave_name(st->effect), a, b2_note(note, b2));
}

// One decoded report: a peer that advertises MP_F_CLIP with a clip id not handed over yet for its address goes to
// clip_cb once (the oldest slot is reused when the table is full)
static void clip_peer(const uint8_t addr[6], const mp_state_t *st)
{
    if (!(st->flags & MP_F_CLIP)) return;
    int k = -1, free_k = -1;
    for (int i = 0; i < MP_MAX_PEERS && k < 0; i++) {
        if (clip_seen[i].used && !memcmp(clip_seen[i].addr, addr, 6)) k = i;
        else if (!clip_seen[i].used && free_k < 0) free_k = i;
    }
    if (k >= 0 && clip_seen[k].id == st->clip_id) return;
    if (k < 0) k = free_k >= 0 ? free_k : (int)(st->origin % MP_MAX_PEERS);
    clip_seen[k].used = true;
    clip_seen[k].id = st->clip_id;
    memcpy(clip_seen[k].addr, addr, 6);
    char a[18];
    fmt_addr(a, sizeof(a), addr);
    ESP_LOGI(TAG, "clip %u from %s, %u.%u s", st->clip_id, a, st->clip_ds / 10, st->clip_ds % 10);
    mesh_clip_cb_t cb = clip_cb;
    if (cb) cb(addr, st->origin, st->clip_id, st->clip_ds);
}

// One decoded report: a new round with time left is joined (rps_join decides), with the reveal at the time heard
static void game_peer(const uint8_t addr[6], const mp_state_t *st, uint32_t now)
{
    if (!st->game_seq || !st->game_left) return;
    xSemaphoreTake(table_lock, portMAX_DELAY);
    bool joined = rps_join(&game, st->game_starter, st->game_seq, st->game_left, now);
    if (joined) game_frozen = false;
    uint8_t own = game.own;
    xSemaphoreGive(table_lock);
    if (!joined) return;
    char a[18];
    fmt_addr(a, sizeof(a), addr);
    ESP_LOGI(TAG, "game join %02x/%u from %s, reveal in %d ms, pick %s", st->game_starter, st->game_seq, a,
             st->game_left * RPS_LEFT_UNIT_MS, rps_pick_name(own));
    refresh_req = true;
}

// Under table_lock: the peers' picks in this round, and the outcome
static void game_freeze_locked(void)
{
    mesh_game_t *g = &game_res;
    memset(g, 0, sizeof(*g));
    uint8_t picks[MP_MAX_PEERS];
    for (int i = 0; i < MP_MAX_PEERS && g->n < MESH_GAME_PEERS; i++) {
        const mp_peer_t *p = &table.peers[i];
        if (!p->used || p->st.game_seq != game.seq || p->st.game_starter != game.starter) continue;
        if (p->st.game_pick < RPS_ROCK || p->st.game_pick > RPS_SCISSORS) continue;
        g->peers[g->n] = (mesh_game_peer_t){ p->st.game_pick, p->st.r, p->st.g, p->st.b };
        picks[g->n++] = p->st.game_pick;
    }
    g->own = game.own;
    g->outcome = rps_outcome(game.own, picks, g->n);
    g->phase = RPS_RESULT;
}

// Every tick: the result once the collect phase is over, the round marked done, and whether the advert's game bytes
// must change (the time left moves every RPS_LEFT_UNIT_MS)
static bool game_tick(uint32_t now)
{
    uint8_t b[4];
    bool logres = false;
    mesh_game_t res;
    xSemaphoreTake(table_lock, portMAX_DELAY);
    rps_advert(&game, now, &b[1], &b[0], &b[2], &b[3]);
    if (rps_phase(&game, now) == RPS_RESULT && !game_frozen) {
        game_freeze_locked();
        game_frozen = true;
        res = game_res;
        logres = true;
    }
    rps_tick(&game, now);
    xSemaphoreGive(table_lock);
    if (logres) {
        int k[4] = { 0 };
        for (int i = 0; i < res.n; i++) k[res.peers[i].pick]++;
        ESP_LOGI(TAG, "game result %s: own %s, peers rock %d paper %d scissors %d", rps_outcome_name(res.outcome),
                 rps_pick_name(res.own), k[RPS_ROCK], k[RPS_PAPER], k[RPS_SCISSORS]);
    }
    return memcmp(b, game_adv, sizeof(b)) != 0;
}

static void mesh_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(TICK_MS);
    TickType_t next_tick = xTaskGetTickCount() + period;
    int ticks = 0;
    uint32_t rep0 = 0, cm0 = 0, rep10 = 0, cm10 = 0;
    after_change();                                  // "mesh: mix #000000 n=0" once at start
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = (int32_t)(next_tick - now) > 0 ? next_tick - now : 0;
        bool dirty = false, woke = false;
        report_t r;
        if (xQueueReceive(q, &r, wait) == pdTRUE) {
            do {
                uint32_t w = (uint32_t)uxQueueMessagesWaiting(q) + 1;   // this one plus what is still waiting
                if (w > q_hwm) q_hwm = w;
                if (w > q_hwm_all) q_hwm_all = w;
                if (r.len == 0) { woke = true; continue; }
                mp_state_t st;
                if (!mp_decode(r.mfg, r.len, &st)) continue;
                xSemaphoreTake(table_lock, portMAX_DELAY);
                dirty |= mp_table_update(&table, r.addr, &st, r.rssi, now_ms());
                xSemaphoreGive(table_lock);
                peer_effect(r.addr, &st, now_ms());
                game_peer(r.addr, &st, now_ms());
                clip_peer(r.addr, &st);
            } while (xQueueReceive(q, &r, 0) == pdTRUE);
        }
        if (effect_req) {                                // mesh_start_wave: the effect into the advert, for 1 s
            effect_req = false;
            own_effect = effect_kind_req;
            own_effect_seq++;
            effect_clear_us = esp_timer_get_time() + MESH_EFFECT_MS * 1000LL;
            refresh_req = true;
        }
        if (refresh_req) {
            refresh_req = false;
            do_refresh();
            dirty = true;
        }
        if (woke) radio_check();
        other_t o;
        while (xQueueReceive(q_other, &o, 0) == pdTRUE) {
            char a[18];
            fmt_addr(a, sizeof(a), o.addr);
            ESP_LOGD(TAG_BLE, "other %s type %u rssi %d evt %u len %u", a, o.addr_type, o.rssi, o.evt, o.len);
        }
        now = xTaskGetTickCount();
        if ((int32_t)(now - next_tick) >= 0) {
            next_tick += period;
            if ((int32_t)(now - next_tick) >= 0) next_tick = now + period;   // fell behind: don't burst
            xSemaphoreTake(table_lock, portMAX_DELAY);
            int n = mp_table_expire(&table, now_ms());
            xSemaphoreGive(table_lock);
            if (n) dirty = true;
            if (effect_clear_us && esp_timer_get_time() >= effect_clear_us) {   // the effect's second is over
                effect_clear_us = 0;
                own_effect = WAVE_NONE;
                do_refresh();
                dirty = true;
            }
            if (game_tick(now_ms())) do_refresh();       // the round's bytes (time left, pick, end)
            bool ended = false;
            uint32_t t = now_ms();
            xSemaphoreTake(table_lock, portMAX_DELAY);
            if (waves_any) {
                bool any = false;
                for (int i = 0; i < WAVE_MAX; i++) {
                    if (!wave_over(&waves[i], t)) any = true;
                    else waves[i].kind = WAVE_NONE;
                }
                if (!any) waves_any = false, ended = true;
            }
            xSemaphoreGive(table_lock);
            if (ended) ESP_LOGI(TAG, "wave end");
            radio_check();
            if (++ticks % (1000 / TICK_MS) == 0) {               // every second
                uint32_t rep = n_reports, cm = n_cm;
                rate_rep_last = rep - rep0;
                rate_cm_last = cm - cm0;
                rep0 = rep;
                cm0 = cm;
                bool dbg = esp_log_level_get(TAG_BLE) >= ESP_LOG_DEBUG;
                dbg_other = dbg;
                if (dbg) ESP_LOGD(TAG_BLE, "reports %lu/s (colormesh %lu/s)", (unsigned long)rate_rep_last,
                                  (unsigned long)rate_cm_last);
                if (ticks % (10000 / TICK_MS) == 0) {            // every 10 s
                    uint32_t cbm = cb_us_max;
                    cb_us_max = 0;
                    if (cbm > cb_us_max_all) cb_us_max_all = cbm;
                    cb_us_max_last = cbm;
                    q_hwm_last = q_hwm;
                    q_hwm = 0;
                    ESP_LOGI(TAG_BLE, "reports %lu/s (colormesh %lu/s)", (unsigned long)((rep - rep10) / 10),
                             (unsigned long)((cm - cm10) / 10));
                    ESP_LOGD(TAG_BLE, "queue max %lu/%d (boot %lu), cb max %lu us (boot %lu), drops %lu",
                             (unsigned long)q_hwm_last, MESH_Q_LEN, (unsigned long)q_hwm_all,
                             (unsigned long)cb_us_max_last, (unsigned long)cb_us_max_all, (unsigned long)n_drops);
                    rep10 = rep;
                    cm10 = cm;
                }
            }
        }
        if (dirty) after_change();
    }
}

// ---- NimBLE host ----

static void on_reset(int reason)
{
    ESP_LOGW(TAG_BLE, "host reset, reason %d", reason);
    synced = false;
    adv_on = false;
    scan_on = false;
    adv_itvl = 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) { ESP_LOGE(TAG_BLE, "no Bluetooth address (%d)", rc); return; }
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc) { ESP_LOGE(TAG_BLE, "address type (%d)", rc); return; }
    uint8_t a[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, a, NULL);
    if (rc) { ESP_LOGE(TAG_BLE, "address copy (%d)", rc); return; }
    memcpy(own_raw, a, 6);
    own_origin = a[0];
    fmt_addr(own_addr, sizeof(own_addr), a);
    ESP_LOGI(TAG_BLE, "address %s", own_addr);
    ESP_LOGI(TAG_BLE, "synced");
    synced = true;
    refresh_req = true;                              // origin known now: encode, then start advertising and scanning
    wake();
}

static void host_task(void *arg)
{
    nimble_port_run();              // returns only after nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ---- test console ----

static void where_ble(char *out, size_t n)
{
    size_t k = strlen(out);
    if (k >= n) return;
    snprintf(out + k, n - k, " ble_rep=%lu ble_cm=%lu ble_q=%lu/%lu ble_cb=%luus ble_drop=%lu ble_adv=%u ble_scan=%d",
             (unsigned long)rate_rep_last, (unsigned long)rate_cm_last, (unsigned long)q_hwm_all,
             (unsigned long)MESH_Q_LEN, (unsigned long)cb_us_max_all, (unsigned long)n_drops,
             (unsigned)adv_itvl, (int)scan_on);
}

// "blelog info|debug": the 1 s rate line, the 10 s queue/callback line and one line per non-ColorMesh report
static void cmd_blelog(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "info") && strcmp(argv[1], "debug"))) {
        ESP_LOGW("test", "error blelog: usage blelog info|debug");
        return;
    }
    esp_log_level_set(TAG_BLE, strcmp(argv[1], "debug") ? ESP_LOG_INFO : ESP_LOG_DEBUG);
    ESP_LOGI("test", "ok blelog %s", argv[1]);
}

// ---- API ----

void mesh_start(void)
{
    const app_cfg_t *c = config_get();
    own_r = c->r;
    own_g = c->g;
    own_b = c->b;
    dev_mode = c->dev_mode;
    admin = c->admin;
    own_b2 = c->b2_set ? 0x01000000u | (uint32_t)c->b2_r << 16 | (uint32_t)c->b2_g << 8 | c->b2_b : 0;
    mp_ver_from_string(esp_app_get_description()->version, own_ver);
    mp_table_init(&table, 0, 0);
    table_lock = xSemaphoreCreateMutex();
    radio_lock = xSemaphoreCreateMutex();
    q = xQueueCreate(MESH_Q_LEN, sizeof(report_t));
    q_other = xQueueCreate(OTHER_Q_LEN, sizeof(other_t));
    if (!table_lock || !radio_lock || !q || !q_other) { ESP_LOGE(TAG, "no memory for the mesh"); return; }
    refresh_req = true;                              // self in the table before sync (origin 0 until then)
    xTaskCreatePinnedToCore(mesh_task, "mesh", 4096, NULL, 5, NULL, 0);
    testcon_add_where(where_ble);
    testcon_register("blelog", "blelog info|debug", cmd_blelog);

    esp_err_t err = nimble_port_init();     // the controller too (init + enable, BLE mode)
    if (err != ESP_OK) { ESP_LOGE(TAG_BLE, "nimble_port_init: %s", esp_err_to_name(err)); return; }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();                     // once
    ble_svc_gap_device_name_set("colormesh");
    // GATT services go here, before the host task starts: mesh_fw_init() registers the firmware-pull service
    // (ble_gatts_count_cfg + ble_gatts_add_svcs) once agent A7 writes it; a stub today
    mesh_fw_init();
    nimble_port_freertos_init(host_task);   // pinned to CONFIG_BT_NIMBLE_PINNED_TO_CORE (0)
}

void mesh_set_holding(bool on)
{
    if (on == holding) return;
    holding = on;
    ESP_LOGI(TAG, "hold %s", on ? "on" : "off");
    refresh_req = true;
    wake();
}

void mesh_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (r == own_r && g == own_g && b == own_b) return;
    own_r = r;
    own_g = g;
    own_b = b;
    refresh_req = true;
    wake();
}

void mesh_set_color2(uint8_t r, uint8_t g, uint8_t b, bool set)
{
    if (set && !(r | g | b)) r = g = b = 1;          // as config_set_color2: black is #010101
    uint32_t v = set ? 0x01000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | b : 0;
    if (v == own_b2) return;
    own_b2 = v;
    refresh_req = true;
    wake();
}

void mesh_set_dev_mode(bool on)
{
    if (on == dev_mode) return;
    dev_mode = on;
    refresh_req = true;
    wake();
}

void mesh_set_admin(bool on)
{
    if (on == admin) return;
    admin = on;
    refresh_req = true;
    wake();
}

void mesh_set_target(uint8_t origin)
{
    if (origin == target) return;
    target = origin;
    refresh_req = true;
    wake();
}

void mesh_refresh(void)
{
    refresh_req = true;
    wake();
}

void mesh_set_connectable(bool on)
{
    if (on == connectable) return;
    connectable = on;
    if (!synced) return;                             // the first start after sync uses the new mode
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    adv_stop_locked();                               // the mode lives in the parameters: stop, then start
    if (!adv_paused) adv_start_locked();
    xSemaphoreGive(radio_lock);
}

void mesh_adv_pause(bool pause)
{
    adv_paused = pause;
    if (!synced) return;
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    if (pause) adv_stop_locked();
    else if (!adv_on) adv_start_locked();
    xSemaphoreGive(radio_lock);
}

void mesh_scan_pause(bool pause)
{
    scan_paused = pause;
    if (!synced) return;
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    if (pause) scan_stop_locked();
    else if (!scan_on) scan_start_locked();
    xSemaphoreGive(radio_lock);
}

void mesh_set_scan_window(uint16_t window)
{
    uint16_t w = window == 0 ? SCAN_WINDOW : window > SCAN_ITVL ? SCAN_ITVL : window;
    if (w == scan_window) return;
    if (!synced) { scan_window = w; return; }
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    scan_window = w;
    if (scan_on) { scan_stop_locked(); if (!scan_paused) scan_start_locked(); }
    xSemaphoreGive(radio_lock);
}

void mesh_get_mix(uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t p = mix_packed;
    *r = (uint8_t)(p >> 16);
    *g = (uint8_t)(p >> 8);
    *b = (uint8_t)p;
}

int mesh_peers(mesh_peer_t *out, int max)
{
    if (!table_lock || !out || max <= 0) return 0;
    int n = 0;
    xSemaphoreTake(table_lock, portMAX_DELAY);
    for (int i = 0; i < MP_MAX_PEERS && n < max; i++) {
        const mp_peer_t *p = &table.peers[i];
        if (!p->used) continue;
        mesh_peer_t *o = &out[n++];
        memcpy(o->addr, p->addr, 6);
        o->origin = p->st.origin;
        o->r = p->st.r;
        o->g = p->st.g;
        o->b = p->st.b;
        o->holding = p->st.flags & MP_F_HOLDING;
        o->dev = p->st.flags & MP_F_DEV;
        o->servable = p->st.flags & MP_F_SERVABLE;
        o->admin = p->st.flags & MP_F_ADMIN;
        memcpy(o->ver, p->st.ver, 4);
        o->rssi = p->rssi;
        o->last_ms = p->last_ms;
        o->target = p->st.target;
    }
    xSemaphoreGive(table_lock);
    return n;
}

const char *mesh_own_addr(void) { return own_addr; }

bool mesh_holding(void) { return holding; }

bool mesh_peer_holding(void)
{
    if (!table_lock) return false;
    bool any = false;
    xSemaphoreTake(table_lock, portMAX_DELAY);
    for (int i = 0; i < MP_MAX_PEERS && !any; i++) any = table.peers[i].used && (table.peers[i].st.flags & MP_F_HOLDING);
    xSemaphoreGive(table_lock);
    return any;
}

const char *mesh_wave_name(uint8_t kind) { return kind <= WAVE_BOTTOM ? wave_names[kind] : "?"; }

uint8_t mesh_wave_kind(const char *name)
{
    if (name && !strcmp(name, "center")) return WAVE_CENTRE;
    for (uint8_t k = WAVE_NONE + 1; name && k <= WAVE_BOTTOM; k++) if (!strcmp(name, wave_names[k])) return k;
    return WAVE_NONE;
}

void mesh_start_wave(uint8_t kind)
{
    if (kind == WAVE_NONE || kind > WAVE_BOTTOM || !table_lock) return;
    uint32_t b2 = own_b2;
    xSemaphoreTake(table_lock, portMAX_DELAY);
    wave_add_locked(kind, own_r, own_g, own_b, b2, now_ms());   // here at once, in its own colours
    xSemaphoreGive(table_lock);
    char note[12];
    ESP_LOGI(TAG, "wave %s from self%s", mesh_wave_name(kind), b2_note(note, b2));
    effect_kind_req = kind;
    effect_req = true;
    wake();
}

void mesh_start_ring(void)
{
    ESP_LOGI(TAG, "ring from self");
    effect_kind_req = MP_EFFECT_RING;
    effect_req = true;
    wake();
}

void mesh_set_clip(uint8_t id, uint8_t ds, bool on)
{
    own_clip_id = id;
    own_clip_ds = ds;
    own_clip_on = on;
    refresh_req = true;
    wake();
}

void mesh_answer(uint8_t caller)
{
    ESP_LOGI(TAG, "answer to %02x", caller);
    own_answer_to = caller;
    effect_kind_req = MP_EFFECT_ANSWER;
    effect_req = true;
    wake();
}

void mesh_set_voice_hooks(mesh_clip_cb_t clip, mesh_ring_cb_t ring, mesh_ring_cb_t answer)
{
    clip_cb = clip;
    ring_cb = ring;
    answer_cb = answer;
}

int mesh_waves(wave_t *out, int max, uint32_t now_ms)
{
    if (!table_lock || !out || max <= 0) return 0;
    int n = 0;
    xSemaphoreTake(table_lock, portMAX_DELAY);
    for (int i = 0; i < WAVE_MAX; i++) {
        if (wave_over(&waves[i], now_ms)) { waves[i].kind = WAVE_NONE; continue; }
        if (n < max && wave_active(&waves[i], now_ms)) out[n++] = waves[i];
    }
    xSemaphoreGive(table_lock);
    return n;
}

bool mesh_game_start(void)
{
    if (!table_lock) return false;
    uint32_t now = now_ms();
    xSemaphoreTake(table_lock, portMAX_DELAY);
    uint8_t s = (uint8_t)(game_seq_own + 1);
    if (!s) s = 1;                                   // seq 0 means no round
    bool ok = rps_start(&game, own_origin, s, now);
    if (ok) game_seq_own = s, game_frozen = false;
    xSemaphoreGive(table_lock);
    if (!ok) return false;
    ESP_LOGI(TAG, "game start %02x/%u, reveal in %d ms", own_origin, s, RPS_COUNT_MS);
    refresh_req = true;
    wake();
    return true;
}

bool mesh_game_pick(uint8_t pick)
{
    if (!table_lock) return false;
    xSemaphoreTake(table_lock, portMAX_DELAY);
    bool ok = rps_pick(&game, pick, now_ms());
    xSemaphoreGive(table_lock);
    if (!ok) return false;
    ESP_LOGI(TAG, "game pick %s", rps_pick_name(pick));
    refresh_req = true;
    wake();
    return true;
}

void mesh_game_state(mesh_game_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!table_lock) return;
    uint32_t now = now_ms();
    xSemaphoreTake(table_lock, portMAX_DELAY);
    rps_phase_t ph = rps_phase(&game, now);
    if (ph == RPS_RESULT && game_frozen) *out = game_res;
    out->phase = ph == RPS_RESULT && !game_frozen ? RPS_COLLECT : ph;   // the mesh task freezes it within a tick
    out->ms_left = rps_ms_left(&game, now);
    out->own = game.own;
    xSemaphoreGive(table_lock);
}

uint8_t mesh_own_origin(void) { return own_origin; }

uint8_t mesh_own_addr_type(void) { return own_addr_type; }

bool mesh_synced(void) { return synced; }

void mesh_set_gap_hook(mesh_gap_hook_t hook) { gap_hook = hook; }

#if MYNEWT_VAL(BLE_EXT_ADV) && MYNEWT_VAL(BLE_PERIODIC_ADV)
// Instance 1: a non-connectable, non-scannable extended set every 100 ms (it carries only the sync info) on 1M, its
// periodic train on 2M (shorter packets)
int mesh_clip_adv_start(uint16_t per_itvl)
{
    if (!synced) return BLE_HS_ENOTSYNCED;
    struct ble_gap_ext_adv_params p = { 0 };
    p.itvl_min = p.itvl_max = 0xA0;
    p.own_addr_type = own_addr_type;
    p.primary_phy = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = BLE_HCI_LE_PHY_2M;
    p.sid = MESH_ADV_CLIP;
    struct ble_gap_periodic_adv_params pp = { 0 };
    pp.itvl_min = pp.itvl_max = per_itvl;
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    if (ble_gap_ext_adv_active(MESH_ADV_CLIP)) ble_gap_ext_adv_stop(MESH_ADV_CLIP);
    int rc = ble_gap_ext_adv_configure(MESH_ADV_CLIP, &p, NULL, gap_event, NULL);
    if (!rc) rc = ble_gap_periodic_adv_configure(MESH_ADV_CLIP, &pp);
    if (!rc) rc = ble_gap_periodic_adv_start(MESH_ADV_CLIP);
    if (!rc) rc = ble_gap_ext_adv_start(MESH_ADV_CLIP, 0, 0);
    xSemaphoreGive(radio_lock);
    if (rc) ESP_LOGW(TAG_BLE, "clip adv start rc=%d", rc);
    else ESP_LOGI(TAG_BLE, "clip periodic set up, %u.%02u ms", per_itvl * 5u / 4u, per_itvl * 125u % 100u);
    return rc;
}

int mesh_clip_adv_data(const uint8_t *data, int len)
{
    struct os_mbuf *m = os_msys_get_pkthdr(len, 0);
    if (!m) return BLE_HS_ENOMEM;
    int rc = os_mbuf_append(m, data, len);
    if (rc) { os_mbuf_free_chain(m); return rc; }
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    rc = ble_gap_periodic_adv_set_data(MESH_ADV_CLIP, m);
    xSemaphoreGive(radio_lock);
    return rc;
}

void mesh_clip_adv_stop(void)
{
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    ble_gap_periodic_adv_stop(MESH_ADV_CLIP);
    if (ble_gap_ext_adv_active(MESH_ADV_CLIP)) ble_gap_ext_adv_stop(MESH_ADV_CLIP);
    xSemaphoreGive(radio_lock);
    ESP_LOGI(TAG_BLE, "clip periodic set down");
}
#else
int mesh_clip_adv_start(uint16_t per_itvl) { return BLE_HS_ENOTSUP; }
int mesh_clip_adv_data(const uint8_t *data, int len) { return BLE_HS_ENOTSUP; }
void mesh_clip_adv_stop(void) {}
#endif

void mesh_get_stats(mesh_stats_t *out)
{
    out->reports_s = rate_rep_last;
    out->colormesh_s = rate_cm_last;
    out->q_max = q_hwm_last;
    out->q_max_all = q_hwm_all;
    out->cb_us_max = cb_us_max_last;
    out->cb_us_max_all = cb_us_max_all;
    out->drops = n_drops;
    out->adv_itvl = adv_itvl;
}
