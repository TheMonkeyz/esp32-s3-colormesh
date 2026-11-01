// Dev-mode firmware pull (see mesh_fw.h). Stub for now.
#include "mesh_fw.h"
#include "esp_log.h"

static const char *TAG = "fw";

void mesh_fw_init(void) {}

void mesh_fw_set_target(uint8_t origin) { ESP_LOGI(TAG, "target %02x", origin); }

int mesh_fw_progress(void) { return -1; }
