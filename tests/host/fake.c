// Host tests: a scripted esp_http_client (one reply for every request, or a reply function per request) and stubs
// for what tested files call. Link it into a test of code that fetches over HTTP (with $(CJSON)/cJSON.c for JSON
// parsers): see fake.h.
#include <stdlib.h>
#include <string.h>
#include "fake.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"

fake_http_t fake_http = { .status = 200 };

struct esp_http_client { esp_http_client_config_t cfg; int status; };

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) { size_t k = n >= size ? size - 1 : n; memcpy(dst, src, k); dst[k] = 0; }
    return n;
}

const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : e == ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : "ERR"; }
int64_t esp_timer_get_time(void) { return 1000; }
esp_err_t esp_crt_bundle_attach(void *conf) { return ESP_OK; }

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    if (fake_http.no_client) return NULL;                // memory short
    esp_http_client_handle_t c = calloc(1, sizeof(*c));
    c->cfg = *cfg;
    fake_http.inits++;
    strlcpy(fake_http.user_agent, cfg->user_agent ? cfg->user_agent : "", sizeof(fake_http.user_agent));
    return c;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    int n = fake_http.requests++;
    strlcpy(fake_http.url, c->cfg.url ? c->cfg.url : "", sizeof(fake_http.url));
    fake_reply_t r = { fake_http.body, fake_http.body ? strlen(fake_http.body) : 0, fake_http.status, fake_http.err };
    if (fake_http.reply) r = fake_http.reply(fake_http.url, n);
    c->status = r.err ? 0 : r.status;
    if (r.err) return r.err;
    const char *b = r.body ? r.body : "";
    for (size_t o = 0; o < r.len && c->cfg.event_handler; o += 1000) {   // in chunks, as the real client
        esp_http_client_event_t e = { .event_id = HTTP_EVENT_ON_DATA, .client = c, .data = (void *)(b + o),
                                      .data_len = r.len - o < 1000 ? (int)(r.len - o) : 1000, .user_data = c->cfg.user_data };
        if (c->cfg.event_handler(&e) != ESP_OK) break;
    }
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { fake_http.cleanups++; free(c); return ESP_OK; }
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url) { c->cfg.url = url; return ESP_OK; }
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c, void *d) { c->cfg.user_data = d; return ESP_OK; }
