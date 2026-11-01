#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
// A scripted reply: body (len bytes, may hold NULs: a PNG), HTTP status, or a transport error
typedef struct { const void *body; size_t len; int status; esp_err_t err; } fake_reply_t;
// The reply every request gets (fake.c): body, HTTP status, or a transport error; no_client = init returns NULL.
// reply (when set) answers each request instead, from its URL and n (requests before it, from 0): a test of code that
// fetches several things (map tiles). Counted: requests, clients made (inits) and cleaned up; kept: the last URL and
// the User-Agent of the last client made.
typedef struct {
    const char *body; int status; esp_err_t err; bool no_client; int requests;
    fake_reply_t (*reply)(const char *url, int n);
    int inits, cleanups;
    char url[512], user_agent[256];
} fake_http_t;
extern fake_http_t fake_http;
