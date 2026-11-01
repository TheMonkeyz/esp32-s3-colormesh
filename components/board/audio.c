// The board's audio (board_audio.h): forge_audio with this board's pins, I2S0 in both directions, the ES7210
// microphones and the ES8311 speaker on the touch I2C bus
#include "board_audio.h"
#include "board.h"
#include "forge_audio.h"
#include "esp_codec_dev_defaults.h"              // ES7210_SEL_MIC1/2

static const forge_audio_cfg_t cfg = {
    .layout = FORGE_AUDIO_ES7210_ES8311,
    .i2s_port = 0,
    .mclk = 42, .bclk = 9, .ws = 45,
    .din = 10,                                   // ES7210 -> ESP32 (the BSP calls it DSIN)
    .dout = 8,                                   // ESP32 -> ES8311 (speaker)
    .sample_rate = 16000,
    .mic_addr = 0x80,                            // ES7210's 8-bit address (0x40), as esp_codec_dev expects
    .mic_select = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
};

bool board_audio_init(bool speaker)
{
    forge_audio_cfg_t c = cfg;
    c.i2c = board_i2c_bus();
    return forge_audio_init(&c, speaker);
}

bool board_mic_open(float gain_db) { return board_audio_init(false) && forge_audio_mic_open(gain_db); }
bool board_mic_read(int16_t *samples, size_t n) { return forge_audio_mic_read(samples, n); }
const void *board_audio_data_if(void) { return forge_audio_data_if(); }
