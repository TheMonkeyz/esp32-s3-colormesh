#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "sdkconfig.h"

#define SETUP_AP_SSID CONFIG_FORGE_SETUP_SSID   // Kconfig (menuconfig "espforge"), forge.json setup_ssid
// The setup network's password: this display's own, made at the first start (NVS "setup"/"pass"), shown on the
// setup screen and in its QR code. (Until v1.12.0 every display had the same published one.)
const char *net_setup_ap_pass(void);
// Copy a network name (at most 32 bytes, as in a Wi-Fi frame) or a password (at most 64) into the driver's fixed
// fields, which aren't NUL-terminated when full
#define NET_SSID_MAX 32
#define NET_PASS_MAX 64
bool net_creds_valid(const char *ssid, const char *pass);   // 1-32 byte name; password empty, 8-63 chars or 64 hex

void net_init(void);             // also NVS (nvs_init) and the test console's "wifi" and "portal" commands
// What a restart after Easy Connect calls (default esp_restart). forge_ota sets ota_restart_when_safe: a restart in
// the first minute after an update rolls the update back.
void net_set_restart(void (*fn)(void));
void net_restart(void);          // restart through that function (after saving new credentials)
bool net_load_creds(char *ssid, size_t sl, char *pass, size_t pl);
void net_clear_creds(void);
void net_begin(const char *ssid, const char *pass);   // start connecting; retries forever in the background
bool net_wait(int timeout_ms);             // true once connected; false after ~8 failed attempts or timeout
bool net_wait_connected(int timeout_ms);   // wait for a connection only (-1 = forever)
bool net_is_connected(void);
void net_start_portal(void);
bool net_save_creds(const char *ssid, const char *pass);
bool net_get_ssid(char *out, size_t n);
bool net_get_ip(char *out, size_t n);
bool net_in_portal(void);
void net_setup_ap_start(void);   // setup AP + captive portal while staying connected
void net_setup_ap_stop(void);
bool net_setup_ap_active(void);
int net_ap_clients(void);        // phones joined to the setup AP
void net_setup_ap_stop_any(void);  // stop the setup AP even during first-time setup (Easy Connect needs the radio)

// Wi-Fi Easy Connect (Android 10+): on_uri gets the DPP URI to show as a QR code; on_done(true, ssid) when a
// phone sent credentials (they're saved and the board restarts 2.5 s later). Callbacks run in the Wi-Fi task.
typedef void (*net_dpp_uri_cb_t)(const char *uri);
typedef void (*net_dpp_done_cb_t)(bool ok, const char *ssid);
bool net_dpp_start(net_dpp_uri_cb_t on_uri, net_dpp_done_cb_t on_done);
void net_dpp_stop(void);
bool net_dpp_active(void);
bool net_dpp_supported(void);   // false: no Easy Connect here (esp_wifi_remote: Wi-Fi through a co-processor); the
                                // app's setup screen then shows only the setup-network page
// Test console ("wifi ..." commands): pretend the saved network is unreachable, without changing the saved credentials
void net_test_offline(void);             // now (until net_test_online or a restart)
void net_test_offline_next_boot(bool short_setup);   // next boot only (RTC flag); short: auto setup for 60 s only
bool net_test_short_setup(void);         // this boot was started that way (offline_setup in main.c)
void net_test_online(void);              // back to the saved network
void net_test_info(char *out, size_t n); // one line: connected, station SSID, setup modes, clients, retries, channel,
                                         // power save, low-latency users, the download slot's holder, queued jobs

// Wi-Fi power save: ESP-IDF's default (WIFI_PS_MIN_MODEM) makes each reply wait for the router's next beacon, ~100 ms
// a request (MeteoBus's radar: 28 small requests took 4.9-6.6 s, 3.1-3.7 s with power save off; L204). A screen that
// downloads much while open asks for low latency (on while it is open, off when it closes); counted, so two such
// screens don't undo each other. Easy Connect holds the radio awake itself and puts this choice back when it stops.
void net_low_latency(bool on);
int net_low_latency_users(void);

// One TLS download at a time: each holds 10-15 KB of internal RAM, and two at once took an app's internal low point
// from ~40 to 29 KB. svc's probes, forge_ota and forge_map take the slot around their requests; an app's own
// downloads should too (docs/MERGING-APPS.md). `who`: a short name for the log and `wifi status` (dl=<who>).
// false = not free within timeout_ms (the holder's name is logged).
bool net_dl_begin(const char *who, int timeout_ms);
void net_dl_end(const char *who);
bool net_dl_busy(void);                  // someone holds it: a low-priority poller waits before its request
const char *net_dl_holder(void);         // its name, "none" when free

// A background worker for small network jobs (a poller's request, a probe): one short-lived task (internal 8 KB
// stack, started for the first job, gone after 30 s idle) runs them one at a time inside the download slot. `who`
// names the job for the log and `wifi status` (bg=<n> waiting or running). false = the queue (8) is full.
bool net_bg_post(void (*fn)(void *arg), void *arg, const char *who);
int net_bg_queued(void);
