idf_component_register(
    SRCS "ota.c" "ota_web.c"
    INCLUDE_DIRS "include"
    REQUIRES forge_core forge_net
    PRIV_REQUIRES app_update esp_https_ota esp_http_client json nvs_flash esp_timer esp_app_format)
