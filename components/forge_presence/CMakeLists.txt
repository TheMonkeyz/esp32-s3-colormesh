# Screen dimming by presence (include/presence.h). No board code: the microphones, the motion sensor, the touch and
# the brightness come in through presence_hooks_t, so an app with its own board uses it too.
idf_component_register(
    SRCS "presence.c" "presence_sm.c" "presence_json.c" "presence_web.c"
    INCLUDE_DIRS "include"
    REQUIRES
    PRIV_REQUIRES forge_core forge_net json nvs_flash esp_http_server)
