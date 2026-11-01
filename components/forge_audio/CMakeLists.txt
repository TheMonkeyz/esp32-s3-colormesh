# Audio for a board with one I2S port shared by its codecs (include/forge_audio.h). Board-free: the board or the app
# gives the I2C bus, the I2S port and pins. Two layouts: ES7210 microphones + ES8311 speaker (Waveshare AMOLED 1.75),
# one ES8311 for both (MeteoBus's Guition ESP32-P4 board).
idf_component_register(
    SRCS "forge_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_i2s esp_codec_dev)
