// The app's settings in NVS (see config.h)
#include "config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_util.h"
#include "board.h"

static const char *TAG = "cfg";
#define NS "cm"

static app_cfg_t cfg = { .r = 0xff, .g = 0x80, .b = 0x00, .dev_mode = true, .name = "", .bright = 80 };

static void apply_bright(void)
{
    display_lock(-1);
    display_brightness((uint8_t)(cfg.bright * 255 / 100));
    display_unlock();
}

void config_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {       // absent before the first save: the defaults
        uint32_t rgb;
        uint8_t u8;
        size_t n = sizeof(cfg.name);
        char name[sizeof(cfg.name)];
        if (nvs_get_u32(h, "rgb", &rgb) == ESP_OK) {
            cfg.r = rgb >> 16;
            cfg.g = rgb >> 8;
            cfg.b = rgb;
        }
        if (nvs_get_u8(h, "dev", &u8) == ESP_OK) cfg.dev_mode = u8 != 0;
        if (nvs_get_str(h, "name", name, &n) == ESP_OK) strlcpy(cfg.name, name, sizeof(cfg.name));
        if (nvs_get_u8(h, "bright", &u8) == ESP_OK) cfg.bright = u8 > 100 ? 100 : u8;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "loaded: color #%02x%02x%02x, dev %s, name \"%s\", bright %u", cfg.r, cfg.g, cfg.b,
             cfg.dev_mode ? "on" : "off", cfg.name, cfg.bright);
    apply_bright();
}

const app_cfg_t *config_get(void) { return &cfg; }

static bool open_rw(nvs_handle_t *h) { return nvs_check(nvs_open(NS, NVS_READWRITE, h), "open " NS); }

static void commit_close(nvs_handle_t h, bool ok)
{
    if (ok) nvs_check(nvs_commit(h), "commit " NS);
    nvs_close(h);
}

void config_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    cfg.r = r;
    cfg.g = g;
    cfg.b = b;
    nvs_handle_t h;
    if (open_rw(&h)) commit_close(h, nvs_check(nvs_set_u32(h, "rgb", (uint32_t)r << 16 | g << 8 | b), NS "/rgb"));
    ESP_LOGI(TAG, "color #%02x%02x%02x", r, g, b);
}

void config_set_dev(bool on)
{
    cfg.dev_mode = on;
    nvs_handle_t h;
    if (open_rw(&h)) commit_close(h, nvs_check(nvs_set_u8(h, "dev", on), NS "/dev"));
    ESP_LOGI(TAG, "dev %s", on ? "on" : "off");
}

void config_set_name(const char *name)
{
    strlcpy(cfg.name, name ? name : "", sizeof(cfg.name));
    nvs_handle_t h;
    if (open_rw(&h)) commit_close(h, nvs_check(nvs_set_str(h, "name", cfg.name), NS "/name"));
    ESP_LOGI(TAG, "name %s", cfg.name);
}

void config_set_bright(uint8_t pct)
{
    cfg.bright = pct > 100 ? 100 : pct;
    nvs_handle_t h;
    if (open_rw(&h)) commit_close(h, nvs_check(nvs_set_u8(h, "bright", cfg.bright), NS "/bright"));
    apply_bright();
    ESP_LOGI(TAG, "bright %u", cfg.bright);
}
