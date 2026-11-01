#pragma once
#include <stdint.h>

// The app's screens: "pick" (a ring of colours: shown at boot), "main" (own colour, the mix, the peers), "system"
// (version, address, memory, peers) and "message". Any task may call these: they take the display lock.
// "pick" | "main" | "system" are the three pages of one pager: a swipe right on main opens the ring, a tap on a
// colour there saves it and slides back to main, a swipe left goes back without a change. The screens poll
// config_get(), mesh_get_mix(), mesh_peers() and mesh_fw_progress() every 50 ms (an LVGL timer), so a setting changed
// from the console shows within 50 ms without a call here.
void ui_init(void);
void ui_show_pick(void);
void ui_show_main(void);
void ui_message(const char *title, const char *body);
void ui_texts_changed(void);                        // the language changed: every label again

// A peer's version bytes (mesh_peer_t.ver: major, minor, patch, kind) as text: "vX.Y.Z", then "-N" (kind 0..99),
// "-rcN" (100..199, N = kind - 100), nothing (200, a release) or "+N" (201+, N = kind - 201: vX.Y.Z-N-gHASH). No lock, no LVGL: any task.
void ui_fmt_ver(const uint8_t ver[4], char *out, int n);
// The page's picture over rows [y0, y0+n) of a wave frame (RGB565 byte-swapped), as the wave loop draws pick, voice
// and game; nothing on main or when no wave is drawn. Display lock held (console "shot wave").
void ui_wave_layer(uint16_t *band, int y0, int n);
