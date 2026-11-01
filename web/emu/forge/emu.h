#pragma once
// The framework's part of the browser emulator (web/emu/forge, README.md there): what an app's emu_main.c calls.
#include <stdbool.h>

// main.c's app_main() as a task (an Emscripten fiber, emu_tasks.c), as ESP-IDF starts it. The Wi-Fi stubs
// (emu_stubs.c) say a saved network is there and joined, so it goes from its start-up to what it does once Wi-Fi is up.
void emu_start_app_main(void);

// The display's LVGL task and its scheduler, never returns: the due tasks until their next wait, the settings page's
// requests (emu_web.c), then LVGL's timers (once board_init() has made the display), giving the browser its turn.
void emu_loop(void);

// One pass of emu_loop(): for an app that writes its own loop (one that doesn't run main.c, as weather_amoled's)
void emu_step(void);

// ?key=value in the page's address (e.g. a link to a given place): true and the value in out, else false
bool emu_param(const char *key, char *out, int n);

void emu_web_poll(void);                          // emu_web.c: serve the settings page's queued requests
