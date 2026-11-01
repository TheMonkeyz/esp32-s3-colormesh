// NVS for the browser: the settings (the language, the app's own) in a small table, saved to the page's localStorage
// (key "<app>_emu_nvs", EMU_APP from forge.json) on each commit, so a visitor's choices survive a reload. Values up to
// 160 bytes, 64 entries, 8 namespaces: enough for settings, not for caches (an app keeps those in memory here).
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "nvs.h"

#define MAX_ENTRIES 64
typedef struct { char ns[16], key[16]; uint32_t len; uint8_t data[160]; } entry_t;
static entry_t tab[MAX_ENTRIES];
static int n;
static bool loaded;
static char ns_of[8][16];                        // handle -> namespace
static int handles;

EM_JS(int, js_load, (void *dst, int max, const char *app), {
    try {
        const s = localStorage.getItem(UTF8ToString(app) + '_emu_nvs');
        if (!s) return 0;
        const b = Uint8Array.from(atob(s), c => c.charCodeAt(0));
        if (b.length > max) return 0;
        HEAPU8.set(b, dst);
        return b.length;
    } catch (e) { return 0; }
});
EM_JS(void, js_save, (const void *src, int len, const char *app), {
    try {
        let s = "";
        const b = HEAPU8.subarray(src, src + len);
        for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
        localStorage.setItem(UTF8ToString(app) + '_emu_nvs', btoa(s));
    } catch (e) {}
});

static void load(void)
{
    if (loaded) return;
    loaded = true;
    int len = js_load(tab, sizeof(tab), EMU_APP);
    n = len / (int)sizeof(entry_t);
}

static entry_t *find(nvs_handle_t h, const char *key)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(tab[i].ns, ns_of[h]) && !strcmp(tab[i].key, key)) return &tab[i];
    return NULL;
}

static esp_err_t put(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    if (len > sizeof(tab[0].data)) return ESP_FAIL;
    entry_t *e = find(h, key);
    if (!e) {
        if (n == MAX_ENTRIES) return ESP_ERR_NO_MEM;
        e = &tab[n++];
        memset(e, 0, sizeof(*e));
        strncpy(e->ns, ns_of[h], sizeof(e->ns) - 1);
        strncpy(e->key, key, sizeof(e->key) - 1);
    }
    e->len = (uint32_t)len;
    memcpy(e->data, v, len);
    return ESP_OK;
}

static esp_err_t get(nvs_handle_t h, const char *key, void *out, size_t len)
{
    entry_t *e = find(h, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->len != len) return ESP_FAIL;
    memcpy(out, e->data, len);
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    (void)mode;
    load();
    for (int i = 0; i < handles; i++) if (!strcmp(ns_of[i], ns)) { *h = i; return ESP_OK; }
    if (handles == 8) return ESP_ERR_NO_MEM;
    strncpy(ns_of[handles], ns, sizeof(ns_of[0]) - 1);
    *h = handles++;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; js_save(tab, n * (int)sizeof(entry_t), EMU_APP); return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    entry_t *e = find(h, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    *e = tab[--n];
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    entry_t *e = find(h, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = e->len; return ESP_OK; }
    if (*len < e->len) return ESP_FAIL;
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len) { return put(h, key, v, len); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    entry_t *e = find(h, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = e->len; return ESP_OK; }
    if (*len < e->len) return ESP_FAIL;
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v) { return put(h, key, v, strlen(v) + 1); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) { return get(h, key, v, 1); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) { return put(h, key, &v, 1); }
esp_err_t nvs_get_i16(nvs_handle_t h, const char *key, int16_t *v) { return get(h, key, v, 2); }
esp_err_t nvs_set_i16(nvs_handle_t h, const char *key, int16_t v) { return put(h, key, &v, 2); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v) { return get(h, key, v, 2); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v) { return put(h, key, &v, 2); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v) { return get(h, key, v, 4); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v) { return put(h, key, &v, 4); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v) { return get(h, key, v, 4); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v) { return put(h, key, &v, 4); }
esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *v) { return get(h, key, v, 8); }
esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v) { return put(h, key, &v, 8); }
