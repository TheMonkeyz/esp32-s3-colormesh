# Board support for the Waveshare ESP32-S3-Touch-AMOLED-1.75. Component name "board": the root CMakeLists.txt picks
# the board directory with EXTRA_COMPONENT_DIRS.
idf_component_register(
    SRCS "board.c" "display.c" "touch.c" "imu.c" "audio.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_driver_i2c
    PRIV_REQUIRES forge_core forge_lvgl esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2s esp_timer esp_codec_dev)
