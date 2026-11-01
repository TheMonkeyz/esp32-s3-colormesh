#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "wave.h"

// The Bluetooth LE mesh: every board advertises its state (colour, holding, dev mode, version) and scans for the
// others' (docs/RESEARCH-BLE.md). Any task may call these.
typedef struct {
    uint8_t addr[6];        // public address, NimBLE order (addr[0] = least significant byte)
    uint8_t origin;         // origin id: the low byte of the address (addr[0])
    uint8_t r, g, b;
    bool holding, dev, servable;
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
// Radio counters (risk 3 of docs/RESEARCH-BLE.md); also in the test console's "where" (ble_rep= ble_cm= ...)
typedef struct {
    uint32_t reports_s, colormesh_s;    // reports per second over the last 10 s window
    uint32_t q_max, q_max_all;          // queue high-water mark: last 10 s window, since boot (of MESH_Q_LEN)
    uint32_t cb_us_max, cb_us_max_all;  // GAP callback cost, max over the last 10 s window, since boot
    uint32_t drops;                     // reports lost to a full queue, since boot
    uint16_t adv_itvl;                  // advertising interval in 0.625 ms units (0x20 = 20 ms), 0 = off
} mesh_stats_t;
void mesh_get_stats(mesh_stats_t *out);
