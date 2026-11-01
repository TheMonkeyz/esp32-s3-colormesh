// Audio for a board with one I2S port shared by its codecs (include/forge_audio.h). The ES7210 + ES8311 layout is
// boards/ws_amoled175's audio.c until v0.6.0; the single-ES8311 layout is MeteoBus's audio_p4.c (its ESP32-P4 board).
#include "forge_audio.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio";

static forge_audio_cfg_t cfg;
static const audio_codec_data_if_t *data_if;
static bool with_speaker;
static esp_codec_dev_handle_t mic;        // ES7210_ES8311: the microphones' device
static esp_codec_dev_handle_t es8311;     // ES8311_BOTH: the one IN_OUT device
static SemaphoreHandle_t lock;            // ES8311_BOTH: the presence and sound tasks start together

bool forge_audio_init(const forge_audio_cfg_t *c, bool speaker)
{
    if (data_if) {
        if (speaker && !with_speaker) ESP_LOGE(TAG, "I2S already open without the speaker: forge_audio_init(cfg, true) first");
        return !speaker || with_speaker;
    }
    if (!c) return false;
    cfg = *c;
    if (cfg.layout == FORGE_AUDIO_ES8311_BOTH) {
        speaker = true;                                           // one device, both directions
        if (!lock) lock = xSemaphoreCreateMutex();
    }
    // One I2S port, the clocks shared: microphones in, and the speaker out only when asked: each direction's DMA
    // buffers are ~5 KB of internal RAM
    i2s_chan_handle_t rx = NULL, tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(cfg.i2s_port, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = 320;
    cc.auto_clear = true;                                         // silence on the speaker output
    if (i2s_new_channel(&cc, speaker ? &tx : NULL, &rx) != ESP_OK) { ESP_LOGE(TAG, "I2S channels failed"); return false; }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = cfg.mclk, .bclk = cfg.bclk, .ws = cfg.ws, .dout = speaker ? cfg.dout : I2S_GPIO_UNUSED,
                      .din = cfg.din },
    };
    if ((tx && i2s_channel_init_std_mode(tx, &sc) != ESP_OK) || i2s_channel_init_std_mode(rx, &sc) != ESP_OK ||
        (tx && i2s_channel_enable(tx) != ESP_OK) || i2s_channel_enable(rx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S setup failed");
        return false;
    }
    audio_codec_i2s_cfg_t icfg = { .port = cfg.i2s_port, .rx_handle = rx, .tx_handle = tx };
    data_if = audio_codec_new_i2s_data(&icfg);
    with_speaker = speaker;
    return data_if != NULL;
}

/* ---- FORGE_AUDIO_ES7210_ES8311: the microphones' own device ---- */

static bool es7210_open(float gain_db)
{
    audio_codec_i2c_cfg_t ccfg = { .port = 0, .addr = cfg.mic_addr, .bus_handle = cfg.i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ccfg);
    if (!ctrl_if) return false;
    es7210_codec_cfg_t ecfg = { .ctrl_if = ctrl_if, .mic_selected = cfg.mic_select };
    const audio_codec_if_t *codec = es7210_codec_new(&ecfg);
    if (!codec) return false;
    esp_codec_dev_cfg_t dcfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_handle_t d = esp_codec_dev_new(&dcfg);
    if (!d) return false;
    esp_codec_dev_set_in_gain(d, gain_db);
    esp_codec_dev_sample_info_t fs = { .sample_rate = cfg.sample_rate, .channel = 2, .bits_per_sample = 16 };
    if (esp_codec_dev_open(d, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphones (ES7210) don't answer");
        esp_codec_dev_delete(d);
        return false;
    }
    mic = d;
    ESP_LOGI(TAG, "microphones open (ES7210, %.0f dB gain, %d Hz)", gain_db, cfg.sample_rate);
    return true;
}

/* ---- FORGE_AUDIO_ES8311_BOTH: one device kept open, created under a lock ---- */

static bool es8311_open_locked(void)
{
    audio_codec_i2c_cfg_t ccfg = { .port = 0, .addr = cfg.mic_addr, .bus_handle = cfg.i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ccfg);
    if (!ctrl_if) return false;
    es8311_codec_cfg_t ecfg = {
        .ctrl_if = ctrl_if, .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH, .pa_pin = -1, .use_mclk = true,
        .no_dac_ref = true,                                       // both slots the microphone, not the speaker's signal
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&ecfg);
    if (!codec) return false;
    esp_codec_dev_cfg_t dcfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_handle_t d = esp_codec_dev_new(&dcfg);
    if (!d) return false;
    esp_codec_dev_sample_info_t fs = { .sample_rate = cfg.sample_rate, .channel = 2, .bits_per_sample = 16 };
    if (esp_codec_dev_open(d, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8311 doesn't answer");
        esp_codec_dev_delete(d);
        return false;
    }
    es8311 = d;
    return true;
}

static bool es8311_up(void)
{
    if (es8311) return true;
    if (!data_if || !lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = es8311 || es8311_open_locked();
    xSemaphoreGive(lock);
    return ok;
}

bool forge_audio_mic_open(float gain_db)
{
    if (!data_if) return false;
    if (cfg.layout == FORGE_AUDIO_ES8311_BOTH) {
        if (!es8311_up()) return false;
        esp_codec_dev_set_in_gain(es8311, gain_db);
        ESP_LOGI(TAG, "microphone ready (the ES8311's input, %.0f dB gain)", gain_db);
        return true;
    }
    return mic || es7210_open(gain_db);
}

bool forge_audio_mic_read(int16_t *samples, size_t n)
{
    esp_codec_dev_handle_t d = cfg.layout == FORGE_AUDIO_ES8311_BOTH ? es8311 : mic;
    return d && esp_codec_dev_read(d, samples, (int)(n * sizeof(int16_t))) == ESP_CODEC_DEV_OK;
}

const void *forge_audio_data_if(void) { return with_speaker ? data_if : NULL; }

void *forge_audio_speaker_dev(void) { return cfg.layout == FORGE_AUDIO_ES8311_BOTH && es8311_up() ? es8311 : NULL; }
