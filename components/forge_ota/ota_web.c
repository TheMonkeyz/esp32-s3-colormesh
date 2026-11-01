// GET / POST /api/update (docs/PROTOCOL.md §4)
#include "ota.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "web.h"

const char *ota_state_name(ota_state_t s)
{
    static const char *n[] = { "idle", "checking", "up_to_date", "available", "downloading", "done", "failed" };
    return s <= OTA_FAILED ? n[s] : "?";
}

// {"current","latest","channel","state","progress","error","pending_verify","uptime_s","rolled_back"?,"notes"?};
// notes (see ota_get_notes) only when an update is offered
static esp_err_t update_get(httpd_req_t *req)
{
    ota_status_t o;
    ota_get_status(&o);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "current", o.current);
    cJSON_AddStringToObject(j, "latest", o.latest);
    cJSON_AddStringToObject(j, "channel", o.channel);
    cJSON_AddStringToObject(j, "state", ota_state_name(o.state));
    cJSON_AddNumberToObject(j, "progress", o.progress);
    cJSON_AddStringToObject(j, "error", o.error);
    cJSON_AddNumberToObject(j, "err", o.err);
    // A fresh update is "pending verify" until confirmed (60 s); a restart before that rolls it back.
    // tools/harness waits for this to turn false before restarting the board.
    cJSON_AddBoolToObject(j, "pending_verify", ota_pending_verify());
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    if (o.rolled_back[0]) cJSON_AddStringToObject(j, "rolled_back", o.rolled_back);
    if (o.state == OTA_AVAILABLE) {
        char *notes = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
        if (notes) {
            ota_get_notes(notes, 3072);
            cJSON_AddStringToObject(j, "notes", notes);
            free(notes);
        }
    }
    return web_send_json(req, j);
}

// {"channel":"stable"|"beta"} and/or {"action":"check"|"install"}
static esp_err_t update_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    const char *ch = cJSON_GetStringValue(cJSON_GetObjectItem(j, "channel"));
    const char *act = cJSON_GetStringValue(cJSON_GetObjectItem(j, "action"));
    if (ch) ota_set_channel(ch);
    if (act && !strcmp(act, "check")) ota_check_now();
    if (act && !strcmp(act, "install")) ota_install();
    cJSON_Delete(j);
    vTaskDelay(pdMS_TO_TICKS(200));                        // let the OTA task pick it up
    return update_get(req);
}

static const web_route_t routes[] = {
    { "/api/update", HTTP_GET,  update_get, .keyed = false },
    { "/api/update", HTTP_POST, update_post, .keyed = true },
};

void ota_web_routes(void) { web_add_routes(routes, sizeof(routes) / sizeof(routes[0])); }
