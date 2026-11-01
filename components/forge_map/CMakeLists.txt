# Street map pictures from map tiles (include/forge_map.h) and the Web Mercator maths (include/forge_geo.h). No LVGL:
# the app shows the RGB565 picture itself. geo.c, map_draw.c and map_slots.c are also built for the host
# (tests/host/test_map.c); map.c is the task.
idf_component_register(
    SRCS "geo.c" "map_draw.c" "map_slots.c" "map_path.c" "map_cache.c" "map_cache_flash.c" "map.c"
    INCLUDE_DIRS "include"
    REQUIRES forge_core forge_net
    PRIV_REQUIRES esp_http_client esp_timer mbedtls esp_partition)
