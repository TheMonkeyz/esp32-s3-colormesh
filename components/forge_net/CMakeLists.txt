idf_component_register(
    SRCS "net.c" "web.c" "tlscert.c" "svc.c" "dl_gate.c" "bgwork.c"
    INCLUDE_DIRS "include"
    REQUIRES forge_core esp_wifi esp_netif esp_event esp_http_server esp_https_server esp_http_client json nvs_flash
    PRIV_REQUIRES dns_server mbedtls esp_timer wpa_supplicant esp_app_format lwip)
