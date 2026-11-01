// The display's LVGL task and its scheduler, in the browser: one thread, so the tasks (fibers, emu_tasks.c), the
// settings page's requests (emu_web.c) and LVGL take turns, and every wait hands control back to the browser
// (emscripten_sleep, ASYNCIFY) so it can paint and deliver the mouse. LVGL never runs while a task does, as the
// display lock guarantees on the board.
#include <emscripten.h>
#include "lvgl.h"
#include "freertos/task.h"
#include "emu.h"

void app_main(void);                               // main.c

static void app_task(void *arg)
{
    (void)arg;
    app_main();                                    // returns or waits forever: its work is in its tasks and timers
}

void emu_start_app_main(void) { xTaskCreate(app_task, "main", 8192, NULL, 1, NULL); }

void emu_step(void)
{
    emu_tasks_run();                               // app_main (at first) and the app's tasks, until their next wait
    emu_web_poll();                                // the settings page's requests, as the display's web server
    uint32_t wait = 15;
    if (lv_is_initialized() && lv_display_get_default()) wait = lv_timer_handler();   // (once board_init() ran)
    emscripten_sleep(wait > 15 ? 15 : wait < 1 ? 1 : wait);
}

void emu_loop(void)
{
    for (;;) emu_step();
}

EM_JS(int, js_param, (const char *key, char *out, int n), {
    const v = new URLSearchParams(location.search).get(UTF8ToString(key));
    if (!v) return 0;
    stringToUTF8(v, out, n);
    return 1;
});

bool emu_param(const char *key, char *out, int n) { return n > 0 && js_param(key, out, n); }
