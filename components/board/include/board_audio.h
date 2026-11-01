#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The board's audio: one I2S port (I2S0, master, 16 kHz, 16-bit stereo) in both directions, as the two codecs share
// its clocks (MCLK 42, BCLK 9, WS 45): DIN 10 from the ES7210 (two microphones, I2C 0x40), DOUT 8 to the ES8311
// (speaker, I2C 0x18, amplifier enable GPIO 46). The speaker side is opened only when asked: each direction's DMA
// buffers take ~5 KB of internal RAM.
// From esp32-s3-rtcquebec's / weather_amoled's presence.c. After board_init() (it shares the board's I2C bus).

// The I2S port: the microphones' direction, and the speaker's too when `speaker` (silent until something plays).
// board_mic_open() opens it without the speaker: an app that plays sound calls board_audio_init(true) before it.
bool board_audio_init(bool speaker);
// The microphones (ES7210, both channels, gain in dB: 30 for a room's background noise), then reads of n int16
// samples (interleaved left/right), blocking until they are there. false = not available / a failed read.
bool board_mic_open(float gain_db);
bool board_mic_read(int16_t *samples, size_t n);
// For an app that drives the speaker (an esp_codec_dev ES8311 device of its own, as weather_amoled's alert chime):
// the shared I2S data interface (a const audio_codec_data_if_t *), NULL before board_audio_init(true)
const void *board_audio_data_if(void);
