idf_component_register(
    SRCS "forge_lvgl.c" "screens.c" "pager.c" "lvgl_mem.c" "slide.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl forge_core forge_net
    PRIV_REQUIRES esp_psram)

# lvgl_mem.c provides LVGL's allocator (CONFIG_LV_USE_CUSTOM_MALLOC). Nothing references it directly, so force the
# linker to keep it (liblvgl is scanned before this library resolves these).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init" "-u lv_malloc_core" "-u lv_realloc_core" "-u lv_free_core")
