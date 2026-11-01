#pragma once
// Browser emulator: the web server's requests and replies, for the settings page's routes (the app's, forge_ota's
// ota_web.c) run by web/emu/emu_web.c: the page's fetch('/api/...') comes there instead of over the network
#include <stddef.h>
#include "esp_err.h"
typedef enum { HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;
typedef enum {
    HTTPD_400_BAD_REQUEST = 400, HTTPD_401_UNAUTHORIZED = 401, HTTPD_404_NOT_FOUND = 404,
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
} httpd_err_code_t;
typedef struct httpd_req {
    const char *uri;               // the path, without the query
    int method;                    // HTTP_GET or HTTP_POST
    size_t content_len;
    const char *body;              // the request's body ("" without one)
    int status;                    // the reply: its status (0 until sent) and body
    char *reply;
} httpd_req_t;
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *s);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg);
