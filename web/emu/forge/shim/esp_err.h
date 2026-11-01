#pragma once
// Browser emulator (as tests/host): the ESP-IDF error type and the codes the firmware uses
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_HTTP_CONNECT 0x7002
#define ESP_ERR_HTTP_FETCH_HEADER 0x7006
#define ESP_ERR_HTTP_EAGAIN 0x7007
const char *esp_err_to_name(esp_err_t e);
#define ESP_ERROR_CHECK(x) do { esp_err_t err_ = (x); (void)err_; } while (0)
