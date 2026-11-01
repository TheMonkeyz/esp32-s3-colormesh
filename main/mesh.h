#pragma once
#include <stdbool.h>
#include <stdint.h>

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
} mesh_peer_t;

void mesh_start(void);                                  // after config_load(): reads config_get()
void mesh_set_holding(bool on);                         // the finger is down on the main screen ("mesh: hold on|off")
void mesh_set_color(uint8_t r, uint8_t g, uint8_t b);   // this board's colour
void mesh_set_dev_mode(bool on);
void mesh_get_mix(uint8_t *r, uint8_t *g, uint8_t *b);  // the mix of every holder, this board included
int mesh_peers(mesh_peer_t *out, int max);              // the peers heard, up to max; returns the count
const char *mesh_own_addr(void);                        // "aa:bb:cc:dd:ee:ff" (most significant first), "" before sync
bool mesh_holding(void);
