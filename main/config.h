#pragma once
#include <stdbool.h>
#include <stdint.h>

// The app's settings, kept in NVS (namespace "cm", one typed key each: never a blob that could change size).
// Setters save at once and log "cfg: ..." lines. They do not touch the mesh: whoever changes the colour or dev mode
// also calls mesh_set_color() / mesh_set_dev_mode().
typedef struct {
    uint8_t r, g, b;        // this board's colour (default #ff8000)
    bool dev_mode;          // dev mode: pulls newer firmware from a peer (default on)
    char name[17];          // a name shown to peers and on the system page (default "")
    uint8_t bright;         // screen brightness, 0..100 % (default 80)
    bool has_color;         // a colour was saved once (NVS "rgb"): boot goes straight to the main screen
    uint8_t b2_r, b2_g, b2_b;   // the waves' colour B when b2_set (NVS "rgb2", absent = automatic)
    bool b2_set;            // false: colour B is automatic, the opposite hue of the colour (wave_opposite)
    // Secure updates (mesh_fw.c, fw_auth.h). The private key never sits here: config_admin_priv() reads it from NVS.
    bool admin;             // this board is the admin: it signs its image for peers out of dev mode (NVS "admin")
    bool has_admin_key;     // a key pair is stored (NVS "adm_priv" + "adm_own")
    uint8_t admin_pub[64];  // the own public key, X || Y (NVS "adm_own"), when has_admin_key
    bool has_trusted;       // an admin's public key is trusted (NVS "adm_pub")
    uint8_t trusted[64];    // that key, X || Y
} app_cfg_t;

void config_load(void);                 // after board_init(): applies the brightness
const app_cfg_t *config_get(void);
void config_set_color(uint8_t r, uint8_t g, uint8_t b);   // "cfg: color #rrggbb"
void config_set_dev(bool on);                             // "cfg: dev on|off"
void config_set_name(const char *name);                   // "cfg: name <name>" (cut to 16 bytes)
void config_set_bright(uint8_t pct);                      // "cfg: bright <pct>" (clamped to 100), applied at once
// The waves' colour B, chosen ("cfg: color2 #rrggbb") or back to automatic ("cfg: color2 auto", the NVS key erased).
// Pure black can't travel in the advert (0,0,0 there means automatic): it is kept as #010101.
void config_set_color2(uint8_t r, uint8_t g, uint8_t b);
void config_clear_color2(void);
// Secure updates. Each logs one "cfg: ..." line with the key's short id only (fw_auth_short_id), never a key.
void config_set_admin(bool on);                                       // "cfg: admin on|off"
// A new key pair (replaces any): "cfg: admin key <short>"; false when NVS refused it (nothing changed then)
bool config_set_admin_key(const uint8_t priv[32], const uint8_t pub[64]);
bool config_admin_priv(uint8_t out[32]);                              // the private key from NVS; zero it after use
bool config_set_trusted(const uint8_t pub[64]);                       // "cfg: trusted <short>"
void config_clear_trusted(void);                                      // "cfg: trusted none"
