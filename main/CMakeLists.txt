idf_component_register(
    SRCS "main.c" "config.c" "mesh.c" "mesh_proto.c" "mesh_fw.c" "fw_auth.c" "ui.c" "console.c" "app_text.c" "wave.c" "rps.c" "rps_icons.c" "clip.c" "adpcm.c" "clip_chunks.c" "ringtone.c" "calls.c" "layer.c"
    INCLUDE_DIRS "."
    REQUIRES board forge_core forge_lvgl lvgl bt nvs_flash app_update esp_partition esp_app_format json esp_timer
             esp_driver_gpio bootloader_support mbedtls espressif__esp_codec_dev
    EMBED_FILES "montserrat.ttf")
