#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "wave.h"
#include "rps.h"

// The Bluetooth LE mesh: every board advertises its state (colour, holding, dev mode, version) and scans for the
// others' (docs/RESEARCH-BLE.md). Any task may call these.
typedef struct {
    uint8_t addr[6];        // public address, NimBLE order (addr[0] = least significant byte)
    uint8_t origin;         // origin id: the low byte of the address (addr[0])
    uint8_t r, g, b;
    bool holding, dev, servable;
    bool admin;             // MP_F_ADMIN: the peer is an admin board (it signs its image for boards out of dev mode)
    uint8_t ver[4];         // major, minor, patch, version kind
    int8_t rssi;
    uint32_t last_ms;       // when it was last heard (ms since boot)
    uint8_t target;         // dev mode: the origin it wants firmware from (MP_TARGET_NONE otherwise)
} mesh_peer_t;
#define MESH_PEER_HAS_TARGET 1

void mesh_start(void);                                  // after config_load(): reads config_get()
void mesh_set_holding(bool on);                         // the finger is down on the main screen ("mesh: hold on|off")
void mesh_set_color(uint8_t r, uint8_t g, uint8_t b);   // this board's colour
// The waves' colour B (set), or automatic (!set: the opposite hue): the own waves use it, and the advert carries it
// (bytes 19-21) so that the peers draw this board's waves with it too
void mesh_set_color2(uint8_t r, uint8_t g, uint8_t b, bool set);
void mesh_set_dev_mode(bool on);
void mesh_set_admin(bool on);                           // the advert's MP_F_ADMIN bit (secure updates, mesh_fw.c)
void mesh_get_mix(uint8_t *r, uint8_t *g, uint8_t *b);  // the mix of every holder, this board included
int mesh_peers(mesh_peer_t *out, int max);              // the peers heard, up to max; returns the count
const char *mesh_own_addr(void);                        // "aa:bb:cc:dd:ee:ff" (most significant first), "" before sync
bool mesh_holding(void);
bool mesh_peer_holding(void);                           // any peer heard holding

// ---- waves (docs/IDEAS.md "Animations over the mesh") ----
// Starts a wave of this board's colour (kind: WAVE_CENTRE..WAVE_BOTTOM) here at once ("mesh: wave <kind> from self")
// and on every board nearby: the advert carries effect = kind and a bumped effect_seq for MESH_EFFECT_MS, then
// effect 0 again. A peer's report with a new (effect != 0, effect_seq) starts its wave here at the time it is heard
// ("mesh: wave <kind> from <addr>") with the peer's colour B (its bytes 19-21; 0, 0, 0 = automatic). Any task; no
// radio work in the caller (the mesh task re-advertises).
#define MESH_EFFECT_MS 1000
void mesh_start_wave(uint8_t kind);
// The active waves (own and peers', colour B included), up to max; expired ones are dropped. "mesh: wave end" when the
// last one ends.
int mesh_waves(wave_t *out, int max, uint32_t now_ms);
const char *mesh_wave_name(uint8_t kind);               // "centre", "left", "right", "top", "bottom", "?"
uint8_t mesh_wave_kind(const char *name);               // the reverse; WAVE_NONE for anything else

// ---- rock paper scissors (rps.h: the round rules) ----
// The round travels in the advert (bytes 22-25); a board that hears a new round joins it ("mesh: game join ...").
// At the end of the collect phase the picks heard decide the result ("mesh: game result ..."). Any task.
#define MESH_GAME_PEERS 16
typedef struct { uint8_t pick, r, g, b; } mesh_game_peer_t;
typedef struct {
    rps_phase_t phase;
    uint32_t ms_left;                                   // to the reveal, during RPS_COUNT
    uint8_t own;                                        // this board's pick (rps_pick_t)
    rps_outcome_t outcome;                              // RPS_RESULT only, else RPS_OUT_NONE
    int n;                                              // RPS_RESULT only: the peers that picked in this round
    mesh_game_peer_t peers[MESH_GAME_PEERS];            // their picks and colours
} mesh_game_t;
bool mesh_game_start(void);                             // a new round from this board (false while one runs)
bool mesh_game_pick(uint8_t pick);                      // the own pick, during the countdown only
void mesh_game_state(mesh_game_t *out);

// ---- radio control for the dev-mode firmware pull (mesh_fw.c). Call from a task, never from a touch/LVGL
// handler: these talk to NimBLE directly (or wake the mesh task, which does). ----
struct ble_gap_event;
void mesh_set_target(uint8_t origin);                   // the peer this board wants firmware from (MP_TARGET_NONE = none)
// Connectable advertising (BLE_GAP_CONN_MODE_UND, non-discoverable) on/off; synchronous stop/start. No Flags AD:
// the 30-byte ColorMesh AD plus a 3-byte Flags AD would not fit the 31-byte legacy payload, and with
// BLE_GAP_DISC_MODE_NON every flag is zero, so the Flags AD may be left out (Core spec Vol 3 Part C 11.1.3).
// A central that connects ends this advertisement; the mesh task restarts it in the mode set here.
void mesh_set_connectable(bool on);
void mesh_adv_pause(bool pause);                        // stop advertising (true) / resume (false); synchronous
void mesh_scan_pause(bool pause);                       // cancel the scan (true, before ble_gap_connect) / resume
void mesh_set_scan_window(uint16_t window);             // scan window in 0.625 ms units within the 100 ms interval (0 = default, continuous); restarts the scan
void mesh_refresh(void);                                // re-read the OTA state (servable/pending), bump seq, re-advertise
uint8_t mesh_own_origin(void);                          // the origin id (addr[0]), 0 before sync
uint8_t mesh_own_addr_type(void);                       // for ble_gap_connect(); valid after sync
bool mesh_synced(void);                                 // the host is synced (advertising and scanning are up)
// Every GAP event of the mesh's own advertising and scanning procedures goes here first (connect, disconnect, MTU,
// ...: the firmware pull's peripheral side). Return value ignored. NULL removes it. Runs in the NimBLE host task.
typedef void (*mesh_gap_hook_t)(struct ble_gap_event *event);
void mesh_set_gap_hook(mesh_gap_hook_t hook);
// Extended advertising instances (CONFIG_BT_NIMBLE_EXT_ADV): 0 = the flood (legacy PDUs), 1 = a clip's periodic set.
// The instance number is also the set's SID.
#define MESH_ADV_FLOOD 0
#define MESH_ADV_CLIP  1
// ---- voice clips and calls (clip.c) ----
// The own advert: clip id and length (bytes 26-27) with MP_F_CLIP while `on` (the periodic train runs)
void mesh_set_clip(uint8_t id, uint8_t ds, bool on);
// A call: effect MP_EFFECT_RING with a bumped effect seq for MESH_EFFECT_MS, like a wave ("mesh: ring from self")
void mesh_start_ring(void);
// The answer to a call from caller (origin): effect MP_EFFECT_ANSWER, byte 28 = caller, for MESH_EFFECT_MS
void mesh_answer(uint8_t caller);
// Called from the mesh task: a peer advertises MP_F_CLIP with a clip id not handed over yet for its address; a peer's
// new call (with its colour); a peer's answer to this board's call (with its colour). Copy and return.
typedef void (*mesh_clip_cb_t)(const uint8_t addr[6], uint8_t origin, uint8_t id, uint8_t ds);
typedef void (*mesh_ring_cb_t)(uint8_t origin, uint8_t r, uint8_t g, uint8_t b);
void mesh_set_voice_hooks(mesh_clip_cb_t clip, mesh_ring_cb_t ring, mesh_ring_cb_t answer);
// The Android app's clip: its clip set (another address, random, and its own SID) carries a beacon AD
// (clip_chunks.h clip_beacon_parse) in its extended advertising data. Called from the NimBLE host task, once per
// (origin, clip id); copy and return.
typedef void (*mesh_beacon_cb_t)(const uint8_t addr[6], uint8_t addr_type, uint8_t sid, uint8_t origin, uint8_t id,
                                 uint8_t ds);
void mesh_set_beacon_hook(mesh_beacon_cb_t cb);
// The clip's periodic set on instance 1 (clip.c): start with the periodic interval in 1.25 ms units, change the
// periodic data (any length up to 247), stop. Synchronous, under the radio lock. 0 = ok, else a NimBLE rc.
int mesh_clip_adv_start(uint16_t per_itvl);
int mesh_clip_adv_data(const uint8_t *data, int len);
void mesh_clip_adv_stop(void);
// Radio counters (risk 3 of docs/RESEARCH-BLE.md); also in the test console's "where" (ble_rep= ble_cm= ...)
typedef struct {
    uint32_t reports_s, colormesh_s;    // reports per second over the last 10 s window
    uint32_t q_max, q_max_all;          // queue high-water mark: last 10 s window, since boot (of MESH_Q_LEN)
    uint32_t cb_us_max, cb_us_max_all;  // GAP callback cost, max over the last 10 s window, since boot
    uint32_t drops;                     // reports lost to a full queue, since boot
    uint16_t adv_itvl;                  // advertising interval in 0.625 ms units (0x20 = 20 ms), 0 = off
} mesh_stats_t;
void mesh_get_stats(mesh_stats_t *out);
