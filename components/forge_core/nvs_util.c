// NVS helpers (see nvs_util.h)
#include "nvs_util.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "nvs";

bool nvs_check(esp_err_t err, const char *what)
{
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS %s: %s", what, esp_err_to_name(err));
    return err == ESP_OK;
}

void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}
