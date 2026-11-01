// esp_http_client for the browser: a request is a fetch(), so the app's own fetching code (with forge_core's http_once
// or esp_http_client directly) runs unchanged. The service must allow cross-origin requests (it answers
// Access-Control-Allow-Origin): a page can't read the answer otherwise, and the request fails (status -1).
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "esp_http_client.h"
#include "freertos/task.h"

struct esp_http_client {
    char *url;
    http_event_handle_cb handler;
    void *user;
    int status;
};

// A request: fetch() started at once, its result collected by polling (js_done), so a wait works the same in the main
// loop (a sleep) and in a task (back to the main loop until the answer is there; an await inside a fiber isn't safe
// with ASYNCIFY). A plain GET, no header of ours: a page can't set User-Agent (the firmware's, svc_user_agent, isn't
// sent; the browser's is), and a custom header would turn every request into a CORS preflight, which a service may
// not answer. The browser's default caching (never no-cache: some services' policies ask for it, OpenStreetMap's).
EM_JS(int, js_start, (const char *url), {
    const id = (Module.emuReq = Module.emuReq || { n: 0, m: {} }).n++;
    const r = Module.emuReq.m[id] = { done: false, status: -1, body: null };
    fetch(UTF8ToString(url)).then(async res => { r.body = new Uint8Array(await res.arrayBuffer()); r.status = res.status; })
        .catch(e => console.warn('fetch failed', UTF8ToString(url), e)).finally(() => { r.done = true; });
    return id;
});
EM_JS(int, js_done, (int id), { return Module.emuReq.m[id].done ? 1 : 0; });
// The body in a malloc'ed buffer (NULL: no answer), *status, *len; the request is forgotten
EM_JS(uint8_t *, js_take, (int id, int *status, int *len), {
    const r = Module.emuReq.m[id];
    delete Module.emuReq.m[id];
    setValue(status, r.status, 'i32');
    setValue(len, r.body ? r.body.length : 0, 'i32');
    if (!r.body) return 0;
    const p = _malloc(r.body.length + 1);
    HEAPU8.set(r.body, p);
    HEAPU8[p + r.body.length] = 0;
    return p;
});

static uint8_t *js_fetch(const char *url, int *status, int *len)
{
    int id = js_start(url);
    while (!js_done(id)) vTaskDelay(5);
    return js_take(id, status, len);
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    struct esp_http_client *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->url = strdup(cfg->url);
    c->handler = cfg->event_handler;
    c->user = cfg->user_data;
    return c;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    int status = -1, len = 0;
    uint8_t *body = js_fetch(c->url, &status, &len);
    c->status = status;
    if (!body) return ESP_ERR_HTTP_CONNECT;
    esp_http_client_event_t e = { .client = c, .user_data = c->user };
    if (c->handler) {
        e.event_id = HTTP_EVENT_ON_CONNECTED;
        c->handler(&e);
        for (int o = 0; o < len; o += 4096) {             // in pieces, as the firmware receives them
            e.event_id = HTTP_EVENT_ON_DATA;
            e.data = body + o;
            e.data_len = len - o < 4096 ? len - o : 4096;
            c->handler(&e);
        }
        e.event_id = HTTP_EVENT_ON_FINISH;
        e.data = NULL;
        e.data_len = 0;
        c->handler(&e);
    }
    free(body);
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    if (!c) return ESP_OK;
    free(c->url);
    free(c);
    return ESP_OK;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url)
{
    free(c->url);
    c->url = strdup(url);
    return ESP_OK;
}

esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c, void *data) { c->user = data; return ESP_OK; }

esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
