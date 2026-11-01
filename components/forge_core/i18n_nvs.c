// The saved display language (see forge_i18n.h)
#include "forge_i18n.h"
#include "nvs.h"
#include "nvs_util.h"

void i18n_load(void)
{
    nvs_handle_t h;
    char code[8];
    size_t n = sizeof(code);
    if (nvs_open("i18n", NVS_READONLY, &h) != ESP_OK) return;   // never saved: English
    if (nvs_get_str(h, "lang", code, &n) == ESP_OK) i18n_set(i18n_from_code(code));
    nvs_close(h);
}

bool i18n_save(void)
{
    nvs_handle_t h;
    if (!nvs_check(nvs_open("i18n", NVS_READWRITE, &h), "open i18n")) return false;
    bool ok = nvs_check(nvs_set_str(h, "lang", i18n_code(i18n_lang())), "i18n/lang") &&
              nvs_check(nvs_commit(h), "i18n commit");
    nvs_close(h);
    return ok;
}
