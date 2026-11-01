idf_component_register(
    SRCS "main.c" "ui.c" "app_text.c"
    INCLUDE_DIRS "."
    REQUIRES board forge_core forge_net forge_ota forge_lvgl forge_presence forge_settings lvgl json esp_driver_gpio esp_wifi
    EMBED_FILES "montserrat.ttf"
    EMBED_TXTFILES "web/index.html")
