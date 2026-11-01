#pragma once
#include <stdbool.h>
#include <stdint.h>

// Dev-mode firmware pull over Bluetooth (docs/RESEARCH-BLE.md, step 5 of the plan), and the signed pull for boards
// out of dev mode from a paired admin board (mesh_fw.c, fw_auth.h)
void mesh_fw_init(void);
void mesh_fw_set_target(uint8_t origin);    // pull from the peer with this origin id (console "fwpull")
int mesh_fw_progress(void);                 // -1 idle, else 0..100

// Secure updates, run in the fw task (the ECDSA needs its stack); each answers one "test: ..." line:
//   ON: a key pair if none is stored, then admin on ("fw: admin on (key <short>)", "test: ok admin on <short>")
//   OFF: admin off, the key kept ("test: ok admin off"); NEW: a new key pair, replacing ("test: ok admin new <short>")
//   SELFTEST: sign and verify a random message with the stored key ("test: ok admin selftest <ms> sign_ms=...")
enum { MESH_FW_ADMIN_ON = 1, MESH_FW_ADMIN_OFF, MESH_FW_ADMIN_NEW, MESH_FW_ADMIN_SELFTEST };
void mesh_fw_admin(int op);
// Trust an admin's public key (X || Y, 64 bytes; checked to be a P-256 point: "test: ok trust <short>…",
// "fw: trusted admin <short>"), or forget it (NULL: "test: ok trust none"). False while the previous one is pending.
bool mesh_fw_trust(const uint8_t pub[64]);
