#pragma once
// Host shim: the parts of esp_http_client the firmware uses. fake.c answers requests from a scripted reply.
#include <stdbool.h>
#include "esp_err.h"
typedef struct esp_http_client *esp_http_client_handle_t;
typedef enum { HTTP_EVENT_ERROR, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT,   // (as ESP-IDF 5.5)
               HTTP_EVENT_HEADER_SENT = HTTP_EVENT_HEADERS_SENT, HTTP_EVENT_ON_HEADER,
               HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED } esp_http_client_event_id_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);
typedef struct {
    const char *url;
    http_event_handle_cb event_handler;
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int timeout_ms, buffer_size, buffer_size_tx;
    const char *user_agent;
    bool keep_alive_enable;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_perform(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c, void *data);
