#pragma once
#include <stdint.h>

// Dev-mode firmware pull over Bluetooth (docs/RESEARCH-BLE.md, step 5 of the plan)
void mesh_fw_init(void);
void mesh_fw_set_target(uint8_t origin);    // pull from the peer with this origin id (console "fwpull")
int mesh_fw_progress(void);                 // -1 idle, else 0..100
