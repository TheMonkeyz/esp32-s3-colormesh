idf_component_register(
    SRCS "version.c" "png_rows.c" "diag.c" "testcon.c" "testcon_registry.c" "i18n.c" "i18n_nvs.c" "nvs_util.c" "textfit.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client nvs_flash esp_timer esp_partition esp_app_format app_update espcoredump spi_flash esp_psram
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_rom)
