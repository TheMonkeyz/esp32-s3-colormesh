#pragma once
#include <stdbool.h>
#include <stdint.h>

// The app's settings, kept in NVS (namespace "cm", one typed key each: never a blob that could change size).
// Setters save at once and log "cfg: ..." lines. They do not touch the mesh: whoever changes the colour or dev mode
// also calls mesh_set_color() / mesh_set_dev_mode().
typedef struct {
    uint8_t r, g, b;        // this board's colour (default #ff8000)
    bool dev_mode;          // dev mode: pulls newer firmware from a peer (default off)
    char name[17];          // a name shown to peers and on the system page (default "")
    uint8_t bright;         // screen brightness, 0..100 % (default 80)
} app_cfg_t;

void config_load(void);                 // after board_init(): applies the brightness
const app_cfg_t *config_get(void);
void config_set_color(uint8_t r, uint8_t g, uint8_t b);   // "cfg: color #rrggbb"
void config_set_dev(bool on);                             // "cfg: dev on|off"
void config_set_name(const char *name);                   // "cfg: name <name>" (cut to 16 bytes)
void config_set_bright(uint8_t pct);                      // "cfg: bright <pct>" (clamped to 100), applied at once
