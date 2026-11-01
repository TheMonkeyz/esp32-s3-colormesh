#pragma once
#include <stdbool.h>
#include "ota.h"

// The starter app's screens: "hello" and "system" (pages of one pager: swipe between them), "setup" (Wi-Fi setup,
// long-press anywhere) and "message" (start-up messages). Any task may call these: they take the display lock.
void ui_init(void);
void ui_home(void);                                 // the pager, on "hello"
void ui_message(const char *title, const char *body);
void ui_texts_changed(void);                        // the language changed: every label again

// Wi-Fi setup: page 1 = the setup network (QR to join it), page 2 = Easy Connect (Android). note: shown at the top
// (NULL: "Tap to cancel" / "Tap to try again"). First-time setup can't be closed.
void ui_wifi_setup(const char *note);
bool ui_wifi_setup_open(void);
bool ui_wifi_setup_close(void);                     // as a tap would, unless a phone is on the setup network
void ui_wifi_setup_end(void);                       // the saved network is back: stop Easy Connect

void ui_ota(const ota_status_t *st);                // forge_ota listener (OTA task)
