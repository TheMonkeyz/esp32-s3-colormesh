// The app's settings in NVS (see config.h)
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_util.h"
#include "board.h"
#include "fw_auth.h"

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
            cfg.has_color = true;
            cfg.r = rgb >> 16;
            cfg.g = rgb >> 8;
            cfg.b = rgb;
        }
        if (nvs_get_u32(h, "rgb2", &rgb) == ESP_OK) {
            cfg.b2_set = true;
            cfg.b2_r = rgb >> 16;
            cfg.b2_g = rgb >> 8;
            cfg.b2_b = rgb;
            if (!(cfg.b2_r | cfg.b2_g | cfg.b2_b)) cfg.b2_r = cfg.b2_g = cfg.b2_b = 1;
        }
        if (nvs_get_u8(h, "dev", &u8) == ESP_OK) cfg.dev_mode = u8 != 0;
        if (nvs_get_str(h, "name", name, &n) == ESP_OK) strlcpy(cfg.name, name, sizeof(cfg.name));
        if (nvs_get_u8(h, "bright", &u8) == ESP_OK) cfg.bright = u8 > 100 ? 100 : u8;
        size_t kn = sizeof(cfg.admin_pub), pn = 0;
        cfg.has_admin_key = nvs_get_blob(h, "adm_own", cfg.admin_pub, &kn) == ESP_OK && kn == sizeof(cfg.admin_pub) &&
                            nvs_get_blob(h, "adm_priv", NULL, &pn) == ESP_OK && pn == FWA_PRIV_LEN;
        if (!cfg.has_admin_key) memset(cfg.admin_pub, 0, sizeof(cfg.admin_pub));
        if (nvs_get_u8(h, "admin", &u8) == ESP_OK) cfg.admin = u8 != 0 && cfg.has_admin_key;
        kn = sizeof(cfg.trusted);
        cfg.has_trusted = nvs_get_blob(h, "adm_pub", cfg.trusted, &kn) == ESP_OK && kn == sizeof(cfg.trusted);
        if (!cfg.has_trusted) memset(cfg.trusted, 0, sizeof(cfg.trusted));
        nvs_close(h);
    }
    char c2[8] = "auto";
    if (cfg.b2_set) snprintf(c2, sizeof(c2), "#%02x%02x%02x", cfg.b2_r, cfg.b2_g, cfg.b2_b);
    char ak[FWA_SHORT_LEN + 1] = "none", tk[FWA_SHORT_LEN + 1] = "none";
    if (cfg.has_admin_key) fwa_short_id(cfg.admin_pub, ak);
    if (cfg.has_trusted) fwa_short_id(cfg.trusted, tk);
    ESP_LOGI(TAG, "loaded: color #%02x%02x%02x, color2 %s, dev %s, name \"%s\", bright %u, admin %s (key %s), trusted %s",
             cfg.r, cfg.g, cfg.b, c2, cfg.dev_mode ? "on" : "off", cfg.name, cfg.bright, cfg.admin ? "on" : "off", ak, tk);
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
    cfg.has_color = true;
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

void config_set_color2(uint8_t r, uint8_t g, uint8_t b)
{
    if (!(r | g | b)) r = g = b = 1;                     // 0,0,0 in the advert means automatic
    cfg.b2_r = r;
    cfg.b2_g = g;
    cfg.b2_b = b;
    cfg.b2_set = true;
    nvs_handle_t h;
    if (open_rw(&h)) commit_close(h, nvs_check(nvs_set_u32(h, "rgb2", (uint32_t)r << 16 | g << 8 | b), NS "/rgb2"));
    ESP_LOGI(TAG, "color2 #%02x%02x%02x", r, g, b);
}

void config_clear_color2(void)
{
    cfg.b2_set = false;
    cfg.b2_r = cfg.b2_g = cfg.b2_b = 0;
    nvs_handle_t h;
    if (open_rw(&h)) {
        esp_err_t err = nvs_erase_key(h, "rgb2");
        commit_close(h, err == ESP_ERR_NVS_NOT_FOUND || nvs_check(err, NS "/rgb2"));
    }
    ESP_LOGI(TAG, "color2 auto");
}

// ---- secure updates ----

void config_set_admin(bool on)
{
    cfg.admin = on && cfg.has_admin_key;
    nvs_handle_t h;
    if (open_rw(&h)) commit_close(h, nvs_check(nvs_set_u8(h, "admin", cfg.admin), NS "/admin"));
    ESP_LOGI(TAG, "admin %s", cfg.admin ? "on" : "off");
}

bool config_set_admin_key(const uint8_t priv[32], const uint8_t pub[64])
{
    nvs_handle_t h;
    if (!open_rw(&h)) return false;
    bool ok = nvs_check(nvs_set_blob(h, "adm_priv", priv, FWA_PRIV_LEN), NS "/adm_priv") &&
              nvs_check(nvs_set_blob(h, "adm_own", pub, FWA_PUB_LEN), NS "/adm_own");
    ok = ok && nvs_check(nvs_commit(h), "commit " NS);
    nvs_close(h);
    if (!ok) return false;
    memcpy(cfg.admin_pub, pub, FWA_PUB_LEN);
    cfg.has_admin_key = true;
    char id[FWA_SHORT_LEN + 1];
    fwa_short_id(pub, id);
    ESP_LOGI(TAG, "admin key %s", id);
    return true;
}

bool config_admin_priv(uint8_t out[32])
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = FWA_PRIV_LEN;
    bool ok = nvs_get_blob(h, "adm_priv", out, &n) == ESP_OK && n == FWA_PRIV_LEN;
    nvs_close(h);
    if (!ok) fwa_zero(out, FWA_PRIV_LEN);
    return ok;
}

bool config_set_trusted(const uint8_t pub[64])
{
    nvs_handle_t h;
    if (!open_rw(&h)) return false;
    bool ok = nvs_check(nvs_set_blob(h, "adm_pub", pub, FWA_PUB_LEN), NS "/adm_pub");
    commit_close(h, ok);
    if (!ok) return false;
    memcpy(cfg.trusted, pub, FWA_PUB_LEN);
    cfg.has_trusted = true;
    char id[FWA_SHORT_LEN + 1];
    fwa_short_id(pub, id);
    ESP_LOGI(TAG, "trusted %s", id);
    return true;
}

void config_clear_trusted(void)
{
    cfg.has_trusted = false;
    memset(cfg.trusted, 0, sizeof(cfg.trusted));
    nvs_handle_t h;
    if (open_rw(&h)) {
        esp_err_t err = nvs_erase_key(h, "adm_pub");
        commit_close(h, err == ESP_ERR_NVS_NOT_FOUND || nvs_check(err, NS "/adm_pub"));
    }
    ESP_LOGI(TAG, "trusted none");
}
