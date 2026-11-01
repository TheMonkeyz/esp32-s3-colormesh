#pragma once
// ColorMesh wire format, peer table and colour mix: pure C99, no ESP-IDF, so the same file builds in the firmware
// and in the host tests (tests/host/test_mesh.c). Time is always an argument (now_ms), never a clock call.
//
// One legacy advertisement carries one manufacturer-data AD structure of MP_AD_SIZE (30) bytes:
//   0 AD length (29) · 1 type 0xFF · 2-3 company id 0xFFFF (little-endian, guarded by the protocol byte) ·
//   4 protocol MP_PROTO · 5 origin (low byte of the address) · 6 seq (bumped on a state change, wrap-aware) ·
//   7 TTL (0 = never relay) · 8 flags (MP_F_*) · 9-11 R G B · 12-14 firmware major minor patch ·
//   15 version kind (MP_VER_*) · 16 target origin (dev mode, else MP_TARGET_NONE) · 17-29 reserved, zero.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MP_PROTO        0x43      // protocol id, byte 4; anything else is rejected
#define MP_COMPANY      0xFFFF    // company id, little-endian, guarded by MP_PROTO
#define MP_AD_SIZE      30        // whole AD structure on air: length byte (29), type 0xFF, 28 value bytes
#define MP_MFG_LEN      28        // value after the AD type (company id first), as ble_hs_adv_find_field gives it:
                                  // mp_decode(f->value, f->length - 1, &st)
#define MP_F_HOLDING    0x01      // the screen is being held: this board's colour is in everyone's mix
#define MP_F_DEV        0x02      // dev mode
#define MP_F_SERVABLE   0x04      // the running image is valid and can be served to a dev-mode peer
#define MP_F_PENDING    0x08      // the running image is still pending verification
#define MP_TARGET_NONE  0xFF
// ver[3], the version kind (same order as forge_core version.h, finer among test labels):
// vX.Y.Z-x.N -> N (0-99) < -rc.N -> 100+N (100-199) < release 200 < vX.Y.Z-N-gHASH -> 201+N (201-255)
#define MP_VER_RC       100
#define MP_VER_RELEASE  200
#define MP_VER_POST     201
#define MP_MAX_PEERS    16
#define MP_HOLD_EXPIRY_MS 250     // 3 x 20 ms + 100 ms margin, rounded; 400 once the 100 ms interval is in use
#define MP_PEER_EXPIRY_MS 10000
#define MP_SEQ_RESET_REPORTS 3    // consecutive "older" reports from one address = it rebooted: adopt its seq
#define MP_ADV_FAST_MS  20
#define MP_ADV_SLOW_MS  100
#define MP_ADV_FAST_BELOW_PEERS 6

typedef struct mp_state { uint8_t origin, seq, ttl, flags, r, g, b, ver[4], target; } mp_state_t;

// The AD structure into buf: MP_AD_SIZE, or -1 if cap is smaller (or an argument is NULL)
int  mp_encode(const mp_state_t *s, uint8_t *buf, int cap);
// The manufacturer-data value (company id first, len MP_MFG_LEN); false on a wrong length, company or protocol
bool mp_decode(const uint8_t *mfg, int len, mp_state_t *out);
// The whole AD structure (length byte first, len MP_AD_SIZE)
bool mp_decode_ad(const uint8_t *ad, int len, mp_state_t *out);

// forge_core's version labels ("v1.2.3", "v1.2.3-rc.4", "v1.2.3-5-gabc", "v1.2.3-fw.6", "v" optional) to
// {major, minor, patch, kind}; numbers above 255 saturate; a label that isn't X.Y.Z[-...] gives {0, 0, 0, 0}
void mp_ver_from_string(const char *v, uint8_t out[4]);
int  mp_ver_cmp(const uint8_t a[4], const uint8_t b[4]);           // <0, 0, >0, lexicographic

typedef struct {
    uint8_t addr[6];
    mp_state_t st;
    int8_t rssi;
    uint32_t last_ms;                 // last report heard, any seq
    bool used;
    uint8_t stale;                    // consecutive reports with an older seq (MP_SEQ_RESET_REPORTS)
} mp_peer_t;
typedef struct {
    mp_peer_t peers[MP_MAX_PEERS];
    mp_state_t self;
    bool self_holding;
    uint32_t hold_expiry_ms, peer_expiry_ms;
} mp_table_t;

void mp_table_init(mp_table_t *t, uint32_t hold_expiry_ms, uint32_t peer_expiry_ms);   // 0 -> the defaults
// One report from addr. True when the peer is new or its colour, flags, version or target changed. A strictly
// older seq is ignored (last_ms and rssi are still refreshed) unless MP_SEQ_RESET_REPORTS come in a row; an equal
// seq re-applies the state. A full table replaces the least recently heard peer.
bool mp_table_update(mp_table_t *t, const uint8_t addr[6], const mp_state_t *st, int8_t rssi, uint32_t now_ms);
// A holder not heard for hold_expiry_ms stops holding; a peer not heard for peer_expiry_ms leaves the table.
// Returns the number of changes (holders cleared + peers removed).
int  mp_table_expire(mp_table_t *t, uint32_t now_ms);
int  mp_table_count(const mp_table_t *t);
// This board's own state (it never hears its own advertisements); also mirrors holding into self.flags
void mp_table_set_self(mp_table_t *t, const mp_state_t *self, bool holding);

// The additive mix of every holder (peers heard holding, plus self while self_holding): summed in linear light,
// scaled so the largest channel is full, back to sRGB. No holders -> 0, 0, 0.
void mp_mix(const mp_table_t *t, uint8_t *r, uint8_t *g, uint8_t *b);
int  mp_holders(const mp_table_t *t);

// Advertising interval: MP_ADV_FAST_MS below MP_ADV_FAST_BELOW_PEERS peers or after a recent change, else
// MP_ADV_SLOW_MS (burst then settle; an interval change is an advertising stop/start)
uint16_t mp_adv_interval_ms(int peers, bool recent_change);
