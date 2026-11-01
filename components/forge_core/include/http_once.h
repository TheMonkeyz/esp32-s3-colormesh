#pragma once
#include "esp_http_client.h"

// One request on its own connection: the client is created, performed and cleaned up. esp_http_client_init() returns
// NULL when memory is short, and perform() / get_status_code() dereference it (a crash, then a reboot): here that is
// ESP_ERR_NO_MEM with status 0, a failed fetch like any other.
static inline esp_err_t http_once(const esp_http_client_config_t *cfg, int *status)
{
    *status = 0;
    esp_http_client_handle_t c = esp_http_client_init(cfg);
    if (!c) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_perform(c);
    *status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return err;
}
