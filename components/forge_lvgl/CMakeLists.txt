idf_component_register(
    SRCS "forge_lvgl.c" "screens.c" "pager.c" "lvgl_mem.c" "slide.c" "slide_slots.c"
    INCLUDE_DIRS "include"
    # No forge_net: screens.h's snapshot picture is forge_core's forge_image_t, so an app with no Wi-Fi leaves forge_net,
    # forge_ota and dns_server out (examples/nonet, check_nonet.py)
    REQUIRES lvgl forge_core
    PRIV_REQUIRES esp_psram esp_driver_usb_serial_jtag)

# lvgl_mem.c provides LVGL's allocator (CONFIG_LV_USE_CUSTOM_MALLOC). Nothing references it directly, so force the
# linker to keep it (liblvgl is scanned before this library resolves these).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init" "-u lv_malloc_core" "-u lv_realloc_core" "-u lv_free_core")
