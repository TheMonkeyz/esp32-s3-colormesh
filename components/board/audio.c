// The board's audio (board_audio.h): I2S0 in both directions, the ES7210 microphones through esp_codec_dev
#include "board_audio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "board.h"

static const char *TAG = "audio";

#define SAMPLE_RATE   16000
#define PIN_MCLK      42
#define PIN_BCLK      9
#define PIN_WS        45
#define PIN_DIN       10                         // ES7210 -> ESP32 (the BSP calls it DSIN)
#define PIN_DOUT      8                          // ESP32 -> ES8311 (speaker)
#define ES7210_ADDR   0x80                       // 8-bit address (0x40), as esp_codec_dev expects

static const audio_codec_data_if_t *data_if;
static esp_codec_dev_handle_t mic;
static bool with_speaker;

bool board_audio_init(bool speaker)
{
    if (data_if) {
        if (speaker && !with_speaker) ESP_LOGE(TAG, "I2S already open without the speaker: board_audio_init(true) first");
        return !speaker || with_speaker;
    }
    // One I2S port, the clocks shared: microphones in (ES7210), and the speaker out (ES8311) only when asked: each
    // direction's DMA buffers are ~5 KB of internal RAM (the starter measured 11 KB for the microphones with both)
    i2s_chan_handle_t rx = NULL, tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = 320;
    cc.auto_clear = true;                                     // silence on the speaker output
    if (i2s_new_channel(&cc, speaker ? &tx : NULL, &rx) != ESP_OK) return false;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = speaker ? PIN_DOUT : I2S_GPIO_UNUSED,
                      .din = PIN_DIN },
    };
    if ((tx && (i2s_channel_init_std_mode(tx, &sc) != ESP_OK)) || i2s_channel_init_std_mode(rx, &sc) != ESP_OK ||
        (tx && i2s_channel_enable(tx) != ESP_OK) || i2s_channel_enable(rx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S setup failed");
        return false;
    }
    audio_codec_i2s_cfg_t icfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    data_if = audio_codec_new_i2s_data(&icfg);
    with_speaker = speaker;
    return data_if != NULL;
}

bool board_mic_open(float gain_db)
{
    if (mic) return true;
    if (!board_audio_init(false)) return false;
    audio_codec_i2c_cfg_t ccfg = { .port = 0, .addr = ES7210_ADDR, .bus_handle = board_i2c_bus() };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ccfg);
    if (!ctrl_if) return false;
    es7210_codec_cfg_t ecfg = { .ctrl_if = ctrl_if, .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 };
    const audio_codec_if_t *codec = es7210_codec_new(&ecfg);
    if (!codec) return false;
    esp_codec_dev_cfg_t dcfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_handle_t d = esp_codec_dev_new(&dcfg);
    if (!d) return false;
    esp_codec_dev_set_in_gain(d, gain_db);
    esp_codec_dev_sample_info_t fs = { .sample_rate = SAMPLE_RATE, .channel = 2, .bits_per_sample = 16 };
    if (esp_codec_dev_open(d, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphones (ES7210) don't answer");
        esp_codec_dev_delete(d);
        return false;
    }
    mic = d;
    ESP_LOGI(TAG, "microphones open (ES7210, %.0f dB gain, %d Hz)", gain_db, SAMPLE_RATE);
    return true;
}

bool board_mic_read(int16_t *samples, size_t n)
{
    return mic && esp_codec_dev_read(mic, samples, n * sizeof(int16_t)) == ESP_CODEC_DEV_OK;
}

const void *board_audio_data_if(void) { return with_speaker ? data_if : NULL; }
