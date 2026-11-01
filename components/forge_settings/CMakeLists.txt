# The Settings screen a long press opens (include/forge_settings.h): rows in sections, ready-made ones for forge_presence,
# the language, the phone's settings page, Wi-Fi, forge_ota and About. Needs LV_USE_QRCODE (the phone page) and the
# board's panel hooks for its slides (forge_lvgl's slide.c; without them LVGL's own animation).
idf_component_register(
    SRCS "settings.c" "settings_std.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES forge_core forge_lvgl forge_net forge_ota forge_presence esp_wifi esp_app_format esp_timer)
