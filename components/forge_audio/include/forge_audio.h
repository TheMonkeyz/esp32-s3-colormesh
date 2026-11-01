#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"

// Audio for a board with one I2S port shared by its codecs, board-free: the board (boards/ws_amoled175's audio.c) or
// an app with its own board code (weather_amoled's main/audio.c) gives the bus and the pins. Two layouts, from the
// boards espforge apps run on:
//  - FORGE_AUDIO_ES7210_ES8311 (Waveshare ESP32-S3-Touch-AMOLED-1.75): ES7210 microphones in; an ES8311 speaker out
//    that the app drives with its own esp_codec_dev device on forge_audio_data_if(), opened and closed around each
//    sound (its amplifier pin is that device's). The speaker's direction is opened only when asked: each direction's
//    DMA buffers are ~5 KB of internal RAM (the starter measured 11 KB for the microphones with both, 6 KB without).
//  - FORGE_AUDIO_ES8311_BOTH (Guition JC1060P470C, an ESP32-P4, MeteoBus's audio_p4.c): one ES8311 is the microphone
//    and the speaker. One esp_codec_dev IN_OUT device kept open, created under a lock (the presence and sound tasks
//    start together): es8311_open() rewrites the chip, so a second device opened at each sound would stop the
//    microphone (LESSONS L212). The app writes its sound to forge_audio_speaker_dev() and switches the amplifier
//    itself (GPIO) around each sound.
typedef enum { FORGE_AUDIO_ES7210_ES8311, FORGE_AUDIO_ES8311_BOTH } forge_audio_layout_t;

typedef struct {
    forge_audio_layout_t layout;
    i2c_master_bus_handle_t i2c;        // the codecs' control bus (the touch bus on both boards)
    int i2s_port;                       // I2S_NUM_0
    int mclk, bclk, ws, din, dout;      // the pins
    int sample_rate;                    // 16000: one clock for the microphones and the speaker
    uint16_t mic_addr;                  // the microphones' codec, 8-bit I2C address as esp_codec_dev expects:
                                        // ES7210 0x80 (0x40), ES8311 0x30 (0x18)
    uint32_t mic_select;                // ES7210 only: ES7210_SEL_MIC1 | ES7210_SEL_MIC2
} forge_audio_cfg_t;

// The I2S port. `speaker`: the output direction too (ES8311_BOTH always opens both). Idempotent (the configuration is
// taken the first time); an app that plays sound calls it with speaker = true before the microphones are opened.
bool forge_audio_init(const forge_audio_cfg_t *cfg, bool speaker);
// The microphones (gain in dB: 30 for a room's background noise), then reads of n int16 samples (interleaved
// left/right), blocking until they are there. false = not available / a failed read. forge_presence's mic_open /
// mic_read hooks.
bool forge_audio_mic_open(float gain_db);
bool forge_audio_mic_read(int16_t *samples, size_t n);
// The shared I2S data interface (a const audio_codec_data_if_t *) for an app's own speaker device: NULL before
// forge_audio_init(cfg, true)
const void *forge_audio_data_if(void);
// ES8311_BOTH only: the one open IN_OUT device (an esp_codec_dev_handle_t) the app writes its sound to, with
// esp_codec_dev_set_out_vol() and esp_codec_dev_write(), never opening or closing it; NULL otherwise or if the
// codec doesn't answer
void *forge_audio_speaker_dev(void);
