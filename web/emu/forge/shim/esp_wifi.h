#pragma once
// Browser emulator: the status page's Wi-Fi line
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint8_t ssid[33]; int8_t rssi; uint8_t primary; } wifi_ap_record_t;
static inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { ap->rssi = -50; ap->primary = 1; return ESP_OK; }
