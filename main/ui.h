#pragma once
#include <stdint.h>

// The app's screens: "pick" (a ring of colours at boot), "main" (own colour, the mix, the peers), "system"
// (version, address, memory, peers) and "message". Any task may call these: they take the display lock.
// "main" and "system" are the two pages of one pager (swipe left / right). The screens poll config_get(),
// mesh_get_mix(), mesh_peers() and mesh_fw_progress() every 50 ms (an LVGL timer), so a setting changed from the
// console shows within 50 ms without a call here.
void ui_init(void);
void ui_show_pick(void);
void ui_show_main(void);
void ui_message(const char *title, const char *body);
void ui_texts_changed(void);                        // the language changed: every label again

// A peer's version bytes (mesh_peer_t.ver: major, minor, patch, kind) as text: "vX.Y.Z", then "-N" (kind 0..99),
// "-rcN" (100..199, N = kind - 100), nothing (200, a release) or "+N" (201+, N = kind - 201: vX.Y.Z-N-gHASH). No lock, no LVGL: any task.
void ui_fmt_ver(const uint8_t ver[4], char *out, int n);
