// The mesh (see mesh.h). Stub for now: NimBLE comes up and reports its address; advertising and scanning come next
// (agent A4), the payload, peer table and mixing are mesh_proto.c (agent A2).
#include "mesh.h"
#include <stdio.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "config.h"

static const char *TAG = "mesh";
static const char *TAG_BLE = "ble";

static volatile bool holding, dev_mode;
static volatile uint8_t own_r, own_g, own_b;
static uint8_t own_addr_type;
static char own_addr[18];

static void on_reset(int reason)
{
    ESP_LOGW(TAG_BLE, "host reset, reason %d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) { ESP_LOGE(TAG_BLE, "no Bluetooth address (%d)", rc); return; }
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc) { ESP_LOGE(TAG_BLE, "address type (%d)", rc); return; }
    uint8_t a[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, a, NULL);
    if (rc) { ESP_LOGE(TAG_BLE, "address copy (%d)", rc); return; }
    snprintf(own_addr, sizeof(own_addr), "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
    ESP_LOGI(TAG_BLE, "address %s", own_addr);
    ESP_LOGI(TAG_BLE, "synced");
}

static void host_task(void *arg)
{
    nimble_port_run();              // returns only after nimble_port_stop()
    nimble_port_freertos_deinit();
}

void mesh_start(void)
{
    const app_cfg_t *c = config_get();
    own_r = c->r;
    own_g = c->g;
    own_b = c->b;
    dev_mode = c->dev_mode;
    esp_err_t err = nimble_port_init();     // the controller too (init + enable, BLE mode)
    if (err != ESP_OK) { ESP_LOGE(TAG_BLE, "nimble_port_init: %s", esp_err_to_name(err)); return; }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();                     // once: GATT services (mesh_fw.c) are added here too, before the host task
    ble_svc_gap_device_name_set("colormesh");
    nimble_port_freertos_init(host_task);   // pinned to CONFIG_BT_NIMBLE_PINNED_TO_CORE (0)
}

void mesh_set_holding(bool on)
{
    if (on == holding) return;
    holding = on;
    ESP_LOGI(TAG, "hold %s", on ? "on" : "off");
}

void mesh_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    own_r = r;
    own_g = g;
    own_b = b;
}

void mesh_set_dev_mode(bool on) { dev_mode = on; }

void mesh_get_mix(uint8_t *r, uint8_t *g, uint8_t *b)
{
    bool h = holding;
    *r = h ? own_r : 0;
    *g = h ? own_g : 0;
    *b = h ? own_b : 0;
}

int mesh_peers(mesh_peer_t *out, int max) { return 0; }

const char *mesh_own_addr(void) { return own_addr; }

bool mesh_holding(void) { return holding; }
