// Voice clips and calls (clip.h). One task owns the microphones, the speaker and the periodic train; the NimBLE host
// task only copies received chunks in (under rx_lock). Buffers are in PSRAM: the recording (mono), the clip being
// sent, the clip being received, and the samples being played.
#include "clip.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "host/ble_hs.h"
#include "board.h"
#include "board_audio.h"
#include "testcon.h"
#include "config.h"
#include "mesh.h"
#include "mesh_proto.h"
#include "adpcm.h"
#include "clip_chunks.h"
#include "ringtone.h"
#include "calls.h"

static const char *TAG = "clip";

#define RATE         16000                  // the board's one I2S clock (board_audio.h)
#define REC_MAX      (RATE * CLIP_REC_MAX_MS / 1000)
#define REC_MIN_MS   300                    // shorter = a tap, not a clip
#define MIC_GAIN_DB  24
#define MIC_FRAMES   256                    // stereo frames per read (16 ms)
#define PEAK_TARGET  26000                  // a quiet clip is scaled up to this peak, at most MAX_BOOST times
#define MAX_BOOST    8
#define PIN_PA       46
#define ES8311_ADDR  0x30                   // 8-bit, as esp_codec_dev expects
#define VOLUME       80
#define RING_AMP     20000                  // the tone's notes sit at 0.3 of this (RMS), its peaks up to it
#define RING_GAP_MS  3200                   // between the phrases, as the old phone
#define SPK_PREROLL_MS 250                   // silence after the speaker opens, before the sound
#define CALLER_NONE  -1
#define PER_ITVL     8                      // periodic interval, 1.25 ms units: 10 ms
#define SYNC_WAIT_MS 4000                   // no sync to the sender's train by then: give up
#define SYNC_TIMEOUT 100                    // 10 ms units: the train is lost after 1 s without a packet

typedef enum { EV_REC_END, EV_REC_DROP, EV_SEND, EV_PLAY_LAST, EV_RING_IN, EV_RING_LOCAL, EV_RX_START, EV_RX_DONE,
               EV_RX_LOST, EV_ANSWERED } ev_kind_t;
typedef struct {
    ev_kind_t kind;
    uint8_t addr[6], origin, id, rgb[3];
    uint8_t addr_type, sid;                 // EV_RX_START: the train's address type and SID (a board: public, 1)
    uint8_t khz;                            // EV_RX_START: the clip's sample rate (a board: 16, the phone: 8)
} ev_t;

static QueueHandle_t q;
static SemaphoreHandle_t rx_lock;
static esp_codec_dev_handle_t spk;
static bool mic_ok;

static int16_t *rec_pcm, *play_pcm;         // REC_MAX mono samples each
static uint8_t *tx_buf, *rx_buf;            // CLIP_MAX_BYTES each
static volatile bool recording, ring_stop, ring_answer;
static call_t call;                         // this board's outgoing call and its answers, under call_mux
static portMUX_TYPE call_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t rec_n;                      // clip task: samples recorded
static uint32_t tx_len;                     // clip task: the last clip's ADPCM bytes (0 = none)
static uint8_t tx_id, tx_ds;

static clip_rx_t rx;                        // under rx_lock
static uint16_t rx_sync = 0xFFFF;           // under rx_lock: the sync handle, 0xFFFF = none
static bool rx_pending;                     // under rx_lock: a sync create is outstanding
static int64_t rx_deadline_us;              // clip task
static uint8_t rx_rgb[3];                   // clip task: the sender's colour
static uint8_t rx_khz = 16;                 // clip task: the clip's sample rate

static clip_state_t st;                     // written by the clip task (and rec_begin), read anywhere

static void post(ev_kind_t k)
{
    ev_t e = { .kind = k };
    if (q) xQueueSend(q, &e, 0);
}

// ---- speaker ----

static bool spk_open(void)
{
    if (spk) return true;
    const audio_codec_data_if_t *data_if = board_audio_data_if();
    if (!data_if) return false;
    audio_codec_i2c_cfg_t ccfg = { .port = 0, .addr = ES8311_ADDR, .bus_handle = board_i2c_bus() };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ccfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!ctrl_if || !gpio_if) return false;
    es8311_codec_cfg_t ecfg = {
        .ctrl_if = ctrl_if, .gpio_if = gpio_if, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_PA, .use_mclk = true, .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&ecfg);
    if (!codec) return false;
    esp_codec_dev_cfg_t dcfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    spk = esp_codec_dev_new(&dcfg);
    return spk != NULL;
}

// n mono samples out, doubled to stereo in blocks
static void spk_write(const int16_t *mono, size_t n)
{
    static int16_t st2[2 * 512];
    for (size_t i = 0; i < n; i += 512) {
        size_t k = n - i < 512 ? n - i : 512;
        for (size_t j = 0; j < k; j++) st2[2 * j] = st2[2 * j + 1] = mono[i + j];
        esp_codec_dev_write(spk, st2, k * 2 * sizeof(int16_t));
    }
}

static void spk_silence(int ms)
{
    static const int16_t zero[256];
    for (int n = RATE * ms / 1000; n > 0; n -= 256) spk_write(zero, n < 256 ? n : 256);
}

static bool spk_begin(void)
{
    if (!spk_open()) { ESP_LOGW(TAG, "no speaker"); return false; }
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 2, .bits_per_sample = 16 };
    if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "speaker open failed"); return false; }
    esp_codec_dev_set_out_vol(spk, VOLUME);
    spk_silence(SPK_PREROLL_MS);                     // the amplifier and the DAC wake up on silence (rc.3 lost the
    return true;                                     // ring's first note, 117 ms, without it)
}

static void spk_end(void) { esp_codec_dev_close(spk); }   // amplifier off between sounds

static void set_phase(clip_phase_t p) { st.phase = p; }

static void play_pcm_n(const int16_t *pcm, uint32_t n, const uint8_t rgb[3])
{
    st.r = rgb[0], st.g = rgb[1], st.b = rgb[2];
    set_phase(CLIP_PLAYING);
    if (spk_begin()) {
        int64_t t0 = esp_timer_get_time();
        spk_write(pcm, n);
        spk_end();
        ESP_LOGI(TAG, "played %lu.%02lu s in %lld ms", (unsigned long)(n / RATE), (unsigned long)(n % RATE * 100 / RATE),
                 (esp_timer_get_time() - t0) / 1000);
    }
    set_phase(CLIP_IDLE);
}

// khz: 16 (a board's clip), or 8 (the phone's: each sample doubled to the I2S rate)
static void play_adpcm(const uint8_t *code, uint32_t len, const uint8_t rgb[3], int khz)
{
    adpcm_state_t s;
    adpcm_init(&s);
    uint32_t max = khz == 8 ? REC_MAX / 4 : REC_MAX / 2;
    if (len > max) len = max;
    adpcm_decode(&s, code, len, play_pcm);
    if (khz == 8) clip_upsample2(play_pcm, len * 2);
    play_pcm_n(play_pcm, khz == 8 ? len * 4 : len * 2, rgb);
}

// A call rings here; `caller` is the calling board's origin (CALLER_NONE for "ring local"). A tap answers: the
// answer goes back through the flood (mesh_answer) so the caller sees it
static void ring(const uint8_t rgb[3], int caller)
{
    st.r = rgb[0], st.g = rgb[1], st.b = rgb[2];
    set_phase(CLIP_RINGING);
    ring_answer = ring_stop = false;
    if (!spk_begin()) { set_phase(CLIP_IDLE); return; }
    int times = 0;
    for (; times < CLIP_RING_TIMES && !ring_stop; times++) {
        ringtone_t t;
        ringtone_init(&t);
        size_t k;
        while (!ring_stop && (k = ringtone_render(&t, play_pcm, 512, RING_AMP)) > 0) spk_write(play_pcm, k);
        for (int g = 0; g < RING_GAP_MS / 16 && !ring_stop; g++) spk_silence(16);   // the gap between phrases
    }
    spk_end();
    bool answered = ring_stop && ring_answer;
    ESP_LOGI(TAG, "ring end after %d, %s", times, answered ? "answered" : ring_stop ? "stopped" : "not answered");
    if (answered && caller != CALLER_NONE) mesh_answer((uint8_t)caller);
    set_phase(CLIP_IDLE);
}

// The caller's "answered" sound (ringtone_answered)
static void play_answered(void)
{
    if (!spk_begin()) return;
    ringtone_t t;
    ringtone_init_seq(&t, ringtone_answered, ringtone_n_answered);
    size_t k;
    while ((k = ringtone_render(&t, play_pcm, 512, RING_AMP)) > 0) spk_write(play_pcm, k);
    spk_end();
    ESP_LOGI(TAG, "answered sound played");
}

// ---- record ----

static void rec_step(void)
{
    static int16_t buf[2 * MIC_FRAMES];
    if (!board_mic_read(buf, 2 * MIC_FRAMES)) return;
    for (int i = 0; i < MIC_FRAMES && rec_n < REC_MAX; i++) rec_pcm[rec_n++] = (int16_t)((buf[2 * i] + buf[2 * i + 1]) / 2);
    st.rec_ms = rec_n * 1000 / RATE;
    if (rec_n >= REC_MAX) {                          // full: end as if the finger went up
        recording = false;
        post(EV_REC_END);
    }
}

// Remove the DC offset, then scale a quiet clip up (speech near a small microphone peaks low)
static void rec_level(void)
{
    int64_t sum = 0;
    for (uint32_t i = 0; i < rec_n; i++) sum += rec_pcm[i];
    int32_t dc = rec_n ? (int32_t)(sum / rec_n) : 0, peak = 1;
    for (uint32_t i = 0; i < rec_n; i++) {
        int32_t v = rec_pcm[i] - dc;
        rec_pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        if (abs(v) > peak) peak = abs(v);
    }
    int32_t num = PEAK_TARGET, den = peak;
    if (num > den * MAX_BOOST) num = den * MAX_BOOST;
    if (num <= den) return;
    for (uint32_t i = 0; i < rec_n; i++) rec_pcm[i] = (int16_t)(rec_pcm[i] * num / den);
    ESP_LOGI(TAG, "level: peak %ld, x%ld.%ld", (long)peak, (long)(num / den), (long)(num * 10 / den % 10));
}

// ---- send ----

static void send_clip(void)
{
    if (!tx_len) { ESP_LOGW(TAG, "no clip to send"); return; }
    uint32_t crc = clip_crc32(tx_buf, tx_len);
    uint16_t total = clip_chunks(tx_len);
    tx_id++;
    set_phase(CLIP_SENDING);
    st.send_pct = 0;
    if (mesh_clip_adv_start(PER_ITVL)) { set_phase(CLIP_IDLE); return; }
    mesh_set_clip(tx_id, tx_ds, true);
    ESP_LOGI(TAG, "send %u: %lu bytes, %u chunks x %d rounds", tx_id, (unsigned long)tx_len, total, CLIP_ROUNDS);
    static uint8_t pkt[CLIP_PKT_MAX];
    int64_t t0 = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();
    int fails = 0;
    for (int r = 0; r < CLIP_ROUNDS; r++) {
        for (uint16_t c = 0; c < total; c++) {
            int n = clip_pack(mesh_own_origin(), tx_id, tx_buf, tx_len, crc, c, pkt);
            if (mesh_clip_adv_data(pkt, n)) fails++;
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(PER_ITVL * 5 / 4));
            st.send_pct = (int)((r * total + c + 1) * 100 / (CLIP_ROUNDS * total));
        }
    }
    mesh_set_clip(tx_id, tx_ds, false);
    mesh_clip_adv_stop();
    ESP_LOGI(TAG, "sent %u in %lld ms, %d data refused", tx_id, (esp_timer_get_time() - t0) / 1000, fails);
    set_phase(CLIP_IDLE);
}

// ---- receive (sync_cb runs in the NimBLE host task) ----

static clip_frag_t frag;                     // host task: a periodic packet in fragments (the Android app's are long)

// One whole periodic packet: every clip chunk AD in it (a board sends one, the Android app several)
static void rx_packet(const uint8_t *data, size_t n)
{
    size_t off = 0, len;
    const uint8_t *ad;
    bool done = false, failed = false;
    xSemaphoreTake(rx_lock, portMAX_DELAY);
    while (!done && clip_next_ad(data, n, &off, &ad, &len)) {
        clip_pkt_t p;
        if (!clip_parse(ad, len, &p)) continue;
        done = clip_rx_add(&rx, &p);
        failed = !done && !rx.active;               // every chunk in but the CRC did not match
        if (failed) break;
    }
    xSemaphoreGive(rx_lock);
    if (done) post(EV_RX_DONE);
    else if (failed) post(EV_RX_LOST);
}

static int sync_cb(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_PERIODIC_SYNC:
        xSemaphoreTake(rx_lock, portMAX_DELAY);
        rx_pending = false;
        if (e->periodic_sync.status == 0) rx_sync = e->periodic_sync.sync_handle;
        xSemaphoreGive(rx_lock);
        if (e->periodic_sync.status) post(EV_RX_LOST);
        break;
    case BLE_GAP_EVENT_PERIODIC_REPORT: {
        int n = clip_frag_add(&frag, e->periodic_report.data, e->periodic_report.data_length,
                              e->periodic_report.data_status);
        if (n > 0) rx_packet(frag.buf, (size_t)n);
        break;
    }
    case BLE_GAP_EVENT_PERIODIC_SYNC_LOST:
        xSemaphoreTake(rx_lock, portMAX_DELAY);
        if (rx_sync == e->periodic_sync_lost.sync_handle) rx_sync = 0xFFFF;
        bool active = rx.active;
        xSemaphoreGive(rx_lock);
        if (active) post(EV_RX_LOST);                // the sender stopped before every chunk came in
        break;
    default:
        break;
    }
    return 0;
}

static void rx_stop(void)
{
    xSemaphoreTake(rx_lock, portMAX_DELAY);
    uint16_t h = rx_sync;
    bool pend = rx_pending;
    rx_sync = 0xFFFF;
    rx_pending = false;
    rx.active = false;
    xSemaphoreGive(rx_lock);
    if (pend) ble_gap_periodic_adv_sync_create_cancel();
    if (h != 0xFFFF) ble_gap_periodic_adv_sync_terminate(h);
    rx_deadline_us = 0;
    st.receiving = false;
}

static void rx_start(const ev_t *e)
{
    if (st.receiving) rx_stop();                     // a newer clip: the old one is dropped
    xSemaphoreTake(rx_lock, portMAX_DELAY);
    clip_rx_begin(&rx, e->origin, e->id, rx_buf);
    rx_pending = true;
    xSemaphoreGive(rx_lock);
    ble_addr_t a = { .type = e->addr_type };
    memcpy(a.val, e->addr, 6);
    clip_frag_reset(&frag);
    struct ble_gap_periodic_sync_params p = { .skip = 0, .sync_timeout = SYNC_TIMEOUT };
    int rc = ble_gap_periodic_adv_sync_create(&a, e->sid, &p, sync_cb, NULL);
    if (rc) {
        ESP_LOGW(TAG, "sync create rc=%d", rc);
        xSemaphoreTake(rx_lock, portMAX_DELAY);
        rx_pending = false;
        rx.active = false;
        xSemaphoreGive(rx_lock);
        return;
    }
    memcpy(rx_rgb, e->rgb, 3);
    rx_khz = e->khz == 8 ? 8 : 16;
    rx_deadline_us = esp_timer_get_time() + SYNC_WAIT_MS * 1000LL;
    st.receiving = true;
}

// ---- mesh hooks (mesh task) ----

static void on_clip(const uint8_t addr[6], uint8_t origin, uint8_t id, uint8_t ds)
{
    ev_t e = { .kind = EV_RX_START, .origin = origin, .id = id, .addr_type = BLE_ADDR_PUBLIC, .sid = MESH_ADV_CLIP,
               .khz = 16 };
    memcpy(e.addr, addr, 6);
    mesh_peer_t peers[MP_MAX_PEERS];
    int n = mesh_peers(peers, MP_MAX_PEERS);
    for (int i = 0; i < n; i++)
        if (!memcmp(peers[i].addr, addr, 6)) e.rgb[0] = peers[i].r, e.rgb[1] = peers[i].g, e.rgb[2] = peers[i].b;
    if (q) xQueueSend(q, &e, 0);
}

// The Android app's clip: its train is on the set that carries the beacon (its address type and SID); the colour is
// the phone's, found by origin (its flood advert has another address)
static void on_beacon(const uint8_t addr[6], uint8_t addr_type, uint8_t sid, uint8_t origin, uint8_t id, uint8_t ds,
                      uint8_t khz)
{
    ev_t e = { .kind = EV_RX_START, .origin = origin, .id = id, .addr_type = addr_type, .sid = sid, .khz = khz };
    memcpy(e.addr, addr, 6);
    mesh_peer_t peers[MP_MAX_PEERS];
    int n = mesh_peers(peers, MP_MAX_PEERS);
    for (int i = 0; i < n; i++)
        if (peers[i].origin == origin) e.rgb[0] = peers[i].r, e.rgb[1] = peers[i].g, e.rgb[2] = peers[i].b;
    if (q) xQueueSend(q, &e, 0);
}

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// A callee answered this board's call (mesh task): one more dot on "Answered", and the doo-di-dit
static void on_answer(uint8_t origin, uint8_t r, uint8_t g, uint8_t b)
{
    taskENTER_CRITICAL(&call_mux);
    bool fresh = call_answer(&call, origin, r, g, b, ms_now());
    int n = call.n;
    taskEXIT_CRITICAL(&call_mux);
    if (!fresh) return;                              // no call out, after the ring, or this board again
    ESP_LOGI(TAG, "call answered by %02x (%d so far)", origin, n);
    post(EV_ANSWERED);
}

static void on_ring(uint8_t origin, uint8_t r, uint8_t g, uint8_t b)
{
    ev_t e = { .kind = EV_RING_IN, .origin = origin, .rgb = { r, g, b } };
    if (q) xQueueSend(q, &e, 0);
}

// ---- task ----

static void handle(const ev_t *e)
{
    switch (e->kind) {
    case EV_REC_END:
    case EV_REC_DROP:
        recording = false;
        if (e->kind == EV_REC_DROP || st.rec_ms < REC_MIN_MS) {
            ESP_LOGI(TAG, "record dropped (%lu ms)", (unsigned long)st.rec_ms);
            set_phase(CLIP_IDLE);
            break;
        }
        rec_n &= ~1u;
        rec_level();
        adpcm_state_t s;
        adpcm_init(&s);
        adpcm_encode(&s, rec_pcm, rec_n, tx_buf);
        tx_len = rec_n / 2;
        tx_ds = (uint8_t)((rec_n * 10 + RATE / 2) / RATE);
        ESP_LOGI(TAG, "recorded %lu ms, %lu bytes", (unsigned long)st.rec_ms, (unsigned long)tx_len);
        send_clip();
        break;
    case EV_SEND:
        send_clip();
        break;
    case EV_PLAY_LAST: {
        const app_cfg_t *c = config_get();
        uint8_t own[3] = { c->r, c->g, c->b };
        if (tx_len) play_adpcm(tx_buf, tx_len, own, 16);
        else ESP_LOGW(TAG, "no clip to play");
        break;
    }
    case EV_RING_IN:
        if (st.phase == CLIP_REC) break;             // busy talking: no ring
        ring(e->rgb, e->origin);
        break;
    case EV_RING_LOCAL: {
        const app_cfg_t *c = config_get();
        uint8_t own[3] = { c->r, c->g, c->b };
        ring(own, CALLER_NONE);
        break;
    }
    case EV_ANSWERED:
        play_answered();
        break;
    case EV_RX_START:
        rx_start(e);
        break;
    case EV_RX_DONE: {
        xSemaphoreTake(rx_lock, portMAX_DELAY);
        uint32_t len = rx.len;
        uint8_t id = rx.id, origin = rx.origin;
        xSemaphoreGive(rx_lock);
        rx_stop();
        ESP_LOGI(TAG, "clip %u from %02x complete, %lu bytes, 0 holes", id, origin, (unsigned long)len);
        play_adpcm(rx_buf, len, rx_rgb, rx_khz);
        break;
    }
    case EV_RX_LOST: {
        xSemaphoreTake(rx_lock, portMAX_DELAY);
        uint16_t miss = clip_rx_missing(&rx);
        uint8_t id = rx.id, origin = rx.origin;
        bool whole = rx.total && rx.have == rx.total;
        xSemaphoreGive(rx_lock);
        if (whole) ESP_LOGW(TAG, "clip %u from %02x: CRC mismatch, dropped", id, origin);
        else if (miss == 0xFFFF) ESP_LOGW(TAG, "clip %u from %02x: no sync, dropped", id, origin);
        else ESP_LOGW(TAG, "clip %u from %02x: %u chunks missing, dropped", id, origin, miss);
        rx_stop();
        break;
    }
    }
}

static void clip_task(void *arg)
{
    for (;;) {
        ev_t e;
        if (recording) {                             // only quick events while the microphones run: a clip to play
            rec_step();                              // or a call waits for the recording to end
            if (xQueueReceive(q, &e, 0) == pdTRUE) {
                bool quick = e.kind == EV_REC_END || e.kind == EV_REC_DROP || e.kind == EV_RX_START || e.kind == EV_RX_LOST;
                if (quick) handle(&e);
                else xQueueSendToBack(q, &e, 0);
            }
            continue;
        }
        if (xQueueReceive(q, &e, pdMS_TO_TICKS(100)) == pdTRUE) handle(&e);
        if (rx_deadline_us && esp_timer_get_time() > rx_deadline_us) {
            xSemaphoreTake(rx_lock, portMAX_DELAY);
            bool synced_now = rx_sync != 0xFFFF;
            xSemaphoreGive(rx_lock);
            if (synced_now) rx_deadline_us = 0;      // the train's own timeout ends it from here
            else { ev_t lost = { .kind = EV_RX_LOST }; handle(&lost); }
        }
    }
}

// ---- console ----

static void cmd_clip(int argc, char **argv)
{
    const char *op = argc > 1 ? argv[1] : "info";
    if (!strcmp(op, "rec")) {
        int ms = argc > 2 ? atoi(argv[2]) : 2000;
        if (ms < REC_MIN_MS || ms > CLIP_REC_MAX_MS) { ESP_LOGW("test", "error clip: rec %d..%d ms", REC_MIN_MS, CLIP_REC_MAX_MS); return; }
        clip_rec_begin();
        vTaskDelay(pdMS_TO_TICKS(ms));
        clip_rec_end(true);
        ESP_LOGI("test", "ok clip rec %d", ms);
    } else if (!strcmp(op, "send")) {
        if (clip_send_last()) ESP_LOGI("test", "ok clip send");
        else ESP_LOGW("test", "error clip: nothing recorded");
    } else if (!strcmp(op, "play")) {
        if (clip_play_last()) ESP_LOGI("test", "ok clip play");
        else ESP_LOGW("test", "error clip: nothing recorded");
    } else if (!strcmp(op, "info")) {
        clip_state_t s;
        clip_get_state(&s);
        ESP_LOGI("test", "ok clip phase=%d last=%u bytes=%lu ds=%u receiving=%d mic=%d spk=%d", s.phase, tx_id,
                 (unsigned long)tx_len, tx_ds, s.receiving, mic_ok, spk != NULL);
    } else {
        ESP_LOGW("test", "error clip: usage clip rec <ms>|send|play|info");
    }
}

static void cmd_ring(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "local")) clip_ring_local();
    else if (argc > 1 && !strcmp(argv[1], "stop")) clip_ring_stop(false);
    else if (argc > 1 && !strcmp(argv[1], "answer")) clip_ring_stop(true);
    else if (argc > 1 && !strcmp(argv[1], "cancel")) clip_call_cancel();
    else if (argc > 1 && !strcmp(argv[1], "answered")) post(EV_ANSWERED);
    else clip_ring();
    ESP_LOGI("test", "ok ring%s%s", argc > 1 ? " " : "", argc > 1 ? argv[1] : "");
}

// ---- API ----

void clip_start(void)
{
    rec_pcm = heap_caps_malloc(REC_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    play_pcm = heap_caps_malloc(REC_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    tx_buf = heap_caps_malloc(CLIP_MAX_BYTES, MALLOC_CAP_SPIRAM);
    rx_buf = heap_caps_malloc(CLIP_MAX_BYTES, MALLOC_CAP_SPIRAM);
    q = xQueueCreate(8, sizeof(ev_t));
    rx_lock = xSemaphoreCreateMutex();
    if (!rec_pcm || !play_pcm || !tx_buf || !rx_buf || !q || !rx_lock) { ESP_LOGE(TAG, "no memory for clips"); return; }
    if (board_audio_init(true)) mic_ok = board_mic_open(MIC_GAIN_DB);   // speaker first (board_audio.h)
    bool sp = spk_open();
    ESP_LOGI(TAG, "microphones %s, speaker %s", mic_ok ? "ready" : "NOT found", sp ? "ready" : "NOT found");
    mesh_set_voice_hooks(on_clip, on_ring, on_answer);
    mesh_set_beacon_hook(on_beacon);
    xTaskCreatePinnedToCore(clip_task, "clip", 4096, NULL, 4, NULL, 0);
    testcon_register("clip", "clip rec <ms>|send|play|info", cmd_clip);
    testcon_register("ring", "ring [local|stop|answer|cancel|answered]", cmd_ring);
}

void clip_rec_begin(void)
{
    if (!mic_ok || st.phase != CLIP_IDLE) return;
    rec_n = 0;
    st.rec_ms = 0;
    set_phase(CLIP_REC);
    recording = true;
    ESP_LOGI(TAG, "record");
}

void clip_rec_end(bool send)
{
    if (st.phase != CLIP_REC) return;
    if (!recording) return;                          // already ended by the time limit
    recording = false;
    post(send ? EV_REC_END : EV_REC_DROP);
}

bool clip_send_last(void)
{
    if (!tx_len) return false;
    post(EV_SEND);
    return true;
}

bool clip_play_last(void)
{
    if (!tx_len) return false;
    post(EV_PLAY_LAST);
    return true;
}

void clip_ring(void)
{
    taskENTER_CRITICAL(&call_mux);
    call_begin(&call, ms_now());
    taskEXIT_CRITICAL(&call_mux);
    mesh_start_ring();
}

void clip_call_cancel(void)
{
    taskENTER_CRITICAL(&call_mux);
    bool was = call.active;
    call_cancel(&call);
    taskEXIT_CRITICAL(&call_mux);
    if (was) ESP_LOGI(TAG, "call screen closed");
}

void clip_ring_local(void) { post(EV_RING_LOCAL); }

void clip_ring_stop(bool answer)
{
    ring_answer = answer;
    ring_stop = true;
}

void clip_get_state(clip_state_t *out)
{
    *out = st;
    taskENTER_CRITICAL(&call_mux);
    bool was = call.active;
    int n0 = call.n;                                 // call_phase clears the call when it ends
    call_phase_t ph = call_phase(&call, ms_now());
    bool ended = was && !call.active;
    out->n_ans = call.n;
    for (int i = 0; i < call.n; i++) out->ans[i] = call.ans[i];
    taskEXIT_CRITICAL(&call_mux);
    if (ended && !n0) ESP_LOGI(TAG, "call not answered");
    else if (ended) ESP_LOGI(TAG, "call over, %d answered", n0);
    out->call = ph == CALL_ANSWERED ? CLIP_CALL_ANSWERED : ph == CALL_OUT ? CLIP_CALL_OUT : CLIP_CALL_NONE;
}
