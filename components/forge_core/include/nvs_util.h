#pragma once
#include <stdbool.h>
#include "esp_err.h"

// NVS calls: log the failure with what was being done, return true on success. The usual pattern:
//   ok = nvs_check(nvs_open("ns", NVS_READWRITE, &h), "open ns") && nvs_check(nvs_set_..., "ns/key") && ...commit
// Settings saved as one blob must never change size (a size change drops the user's settings on update): add new
// settings as separate keys instead.
bool nvs_check(esp_err_t err, const char *what);
// nvs_flash_init(), erasing and retrying when the partition is full or from a newer layout
void nvs_init(void);
