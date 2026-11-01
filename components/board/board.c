// Board bring-up and its test console / diagnostics hooks (see board.h)
#include "board.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "testcon.h"
#include "diag.h"
#include "forge_lvgl.h"
#include "sdkconfig.h"
#include "esp_private/esp_clk.h"           // esp_clk_cpu_freq(): "power"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

void display_init(void);
void touch_init(void);
void touch_register_lvgl(void);
void display_get_test_stats(display_stats_t *out, bool reset);
int display_lvgl_inflight(void);
void display_register_cmds(void);
extern volatile int disp_phase;

// "fps reset", then "fps": frames since the reset (the harness measures animations this way)
static void cmd_fps(int argc, char **argv)
{
    display_stats_t d;
    display_get_test_stats(&d, true);
    if (argc == 2 && !strcmp(argv[1], "reset")) { ESP_LOGI("test", "ok fps reset"); return; }
    ESP_LOGI("test", "fps frames=%lu render_avg_ms=%.1f render_max_ms=%.1f anim_frames=%lu anim_fps=%.1f "
                     "gap_max_ms=%.1f gap_max_at_ms=%lu gap_max_kind=%s mpx=%.2f pushes=%lu push_avg_ms=%.1f "
                     "push_max_ms=%.1f te_wait_avg_ms=%.1f te_wait_max_ms=%.1f te_lag_max_ms=%.2f te_timeouts=%lu "
                     "te_chased=%lu",
             (unsigned long)d.frames, d.frames ? d.render_us / 1000.0f / d.frames : 0, d.render_max_us / 1000.0f,
             (unsigned long)d.anim_frames, d.anim_us ? d.anim_frames * 1e6f / d.anim_us : 0,
             d.anim_gap_max_us / 1000.0f, (unsigned long)d.anim_gap_max_at_ms,
             d.anim_gap_max_kind[0] ? d.anim_gap_max_kind : "-", d.pixels / 1e6f, (unsigned long)d.pushes,
             d.pushes ? d.push_us / 1000.0f / d.pushes : 0, d.push_max_us / 1000.0f,
             d.pushes ? d.te_wait_us / 1000.0f / d.pushes : 0, d.te_wait_max_us / 1000.0f, d.te_lag_max_us / 1000.0f,
             (unsigned long)d.te_timeouts, (unsigned long)d.te_chased);
}

// "power": the panel's state and display.c's "screen" lock (held: 240 MHz even when idle; free: an idle core may drop
// to 80 MHz). cpu_mhz is the clock while this command runs: always the maximum, since a running task holds ESP-IDF's
// own lock (L202): the harness checks screen_lock, not cpu_mhz (presence.dim_off_wake).
static void cmd_power(int argc, char **argv)
{
    ESP_LOGI("test", "power cpu_mhz=%d panel=%s screen_lock=%d", (int)(esp_clk_cpu_freq() / 1000000),
             display_asleep() ? "asleep" : "awake", display_cpu_full_speed());
}

// Breadcrumbs for "where" (no lock): what the display code is doing now
static void where_display(char *out, size_t n)
{
    extern volatile int raw_band;
    snprintf(out, n, " disp_phase=%d raw_band=%d lvgl_inflight=%d", disp_phase, raw_band, display_lvgl_inflight());
}

static void diag_display(void)
{
    display_stats_t d;
    display_get_stats(&d, true);
    ESP_LOGI("diag", "display: %lu frames, render avg %.1f ms max %.1f ms, %.1f Mpx sent | "
             "animation %.1f fps (%lu frames, worst gap %.0f ms) | LVGL lock wait max %.1f ms, "
             "longest hold %.1f ms by %s",
             (unsigned long)d.frames, d.frames ? d.render_us / 1000.0 / d.frames : 0, d.render_max_us / 1000.0,
             d.pixels / 1e6, d.anim_us ? d.anim_frames * 1e6 / d.anim_us : 0, (unsigned long)d.anim_frames,
             d.anim_gap_max_us / 1000.0, d.lvgl_wait_max_us / 1000.0, d.hold_max_us / 1000.0,
             d.hold_task[0] ? d.hold_task : "-");
    ESP_LOGI("diag", "display: %lu pushes, push avg %.1f ms max %.1f ms, TE wait avg %.1f ms max %.1f ms, "
             "TE timeouts %lu", (unsigned long)d.pushes, d.pushes ? d.push_us / 1000.0 / d.pushes : 0,
             d.push_max_us / 1000.0, d.pushes ? d.te_wait_us / 1000.0 / d.pushes : 0, d.te_wait_max_us / 1000.0,
             (unsigned long)d.te_timeouts);
}

// Direct panel and touch access for forge_lvgl's moves drawn as pictures (slide.h)
static const forge_panel_t panel = {
    .raw_frame = display_raw_frame,
    .set_flush_hook = display_set_flush_hook,
    .set_read_hook = touch_set_read_hook,
    .touch_get = touch_get,
    .touch_fresh = touch_fresh,
    .touch_forget = touch_forget,
    .raw_area = display_raw_area,
};

void board_init(void)
{
    ESP_LOGI("board", "%s", BOARD_NAME);
#if CONFIG_PM_ENABLE
    // Dynamic frequency scaling: a core runs at 240 MHz while a task runs on it (ESP-IDF's own "rtos" locks) and may
    // idle at 80 MHz; display.c's "screen" lock keeps 240 even when idle while the panel is awake, so nothing the user
    // sees waits for the clock to come back up. No light sleep: it would break the 15 ms touch / LVGL loop and the
    // panel's DMA timing. 80 MHz keeps the PLL, so APB (SPI, I2S for the microphones, UART) stays at 80 MHz.
    ESP_ERROR_CHECK(esp_pm_configure(&(esp_pm_config_t){ .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false }));
#endif
    touch_init();
    display_init();
    display_lock(-1);
    touch_register_lvgl();
    display_unlock();
    forge_lvgl_init(display_lock, display_unlock);
    forge_lvgl_set_panel(&panel);
    testcon_register("fps", "fps [reset]", cmd_fps);
    testcon_register("power", "power", cmd_power);
    display_register_cmds();
    testcon_add_where(where_display);
    diag_add_hook(diag_display);
}
