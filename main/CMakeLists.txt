idf_component_register(
    SRCS "main.c" "config.c" "mesh.c" "mesh_proto.c" "mesh_fw.c" "ui.c" "console.c" "app_text.c"
    INCLUDE_DIRS "."
    REQUIRES board forge_core forge_lvgl lvgl bt nvs_flash app_update esp_partition esp_app_format json esp_timer
             esp_driver_gpio bootloader_support mbedtls
    EMBED_FILES "montserrat.ttf")
