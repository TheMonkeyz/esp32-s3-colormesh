// CST9217 capacitive touch (I2C 0x5A) -> LVGL pointer input
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "forge_lvgl.h"

static const char *TAG = "touch";

#define PIN_SDA   15
#define PIN_SCL   14
#define PIN_RST   40
#define ADDR      0x5A

static i2c_master_dev_handle_t dev;
static i2c_master_bus_handle_t bus;
static bool ok;
static int n_ok, n_err, n_nofinger, n_status;   // "up" answers: no finger in the report / a status other than contact
static volatile int64_t last_down_us;            // touch_idle_ms()

void touch_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) { ESP_LOGE(TAG, "I2C bus init failed"); return; }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR, .scl_speed_hz = 400000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &dev));

    gpio_config_t rc = { .pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rc);
    gpio_set_level(PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(60));

    ok = i2c_master_probe(bus, ADDR, 50) == ESP_OK;
    esp_log_level_set("i2c.master", ESP_LOG_NONE);   // the controller NACKs while idle; that's normal
    ESP_LOGI(TAG, "CST9217 %s", ok ? "found" : "NOT found");
}

uint32_t touch_idle_ms(void) { return (uint32_t)((esp_timer_get_time() - last_down_us) / 1000); }
i2c_master_bus_handle_t board_i2c_bus(void) { return bus; }

// 1 = pressed, 0 = not pressed, -1 = bus error (keep the previous state)
static int read_chip(int *x, int *y)
{
    bool down;
    if (finger_injected(x, y, &down)) return down;      // the test console's simulated finger
    if (!ok) return 0;
    uint8_t reg[2] = {0xD0, 0x00};
    uint8_t d[10] = {0};
    if (i2c_master_transmit_receive(dev, reg, 2, d, sizeof(d), 20) != ESP_OK) { n_err++; return -1; }
    // Acknowledge every report: without it the controller stops reporting
    uint8_t ack[3] = {0xD0, 0x00, 0xAB};
    i2c_master_transmit(dev, ack, 3, 20);
    n_ok++;
    if (d[6] != 0xAB) return -1;
    if ((d[5] & 0x7F) == 0) { n_nofinger++; return 0; }
    if ((d[0] & 0x0F) != 0x06) { n_status++; return 0; }
    int rx = (d[1] << 4) | (d[3] >> 4);
    int ry = (d[2] << 4) | (d[3] & 0x0F);
    // The panel is mounted rotated 180° relative to the touch sensor (mirror X and Y)
    *x = DISP_W - 1 - rx;
    *y = DISP_H - 1 - ry;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    return 1;
}

static bool fresh;
bool touch_fresh(void) { return fresh; }

int touch_get(int *x, int *y)
{
    static int64_t last;
    static int lr, lx, ly;
    int64_t now = esp_timer_get_time();
    bool down;
    fresh = finger_injected(x, y, &down) || !last || now - last >= 10000;
    if (!fresh) { *x = lx; *y = ly; return lr; }
    last = now;
    lr = read_chip(x, y);
    if (lr > 0) last_down_us = now;
    lx = *x;
    ly = *y;
    return lr;
}

// A drag drawn outside LVGL consumed the touch. Without this, a bus error right after it made read_cb "hold" the last
// point LVGL had seen (the NACK guard below): a fake press there, then a release, so a tap where the drag had started.
static volatile bool forget;
void touch_forget(void) { forget = true; }

static touch_read_hook_t read_hook;
void touch_set_read_hook(touch_read_hook_t hook) { read_hook = hook; }
static touch_press_filter_t press_filter;
void touch_set_press_filter(touch_press_filter_t filter) { press_filter = filter; }

// LVGL reads every ~15-30 ms. Don't poll this chip much faster from elsewhere: read every millisecond (each read is
// acknowledged) it answered "not in contact" for long stretches with the finger on it (weather_amoled v1.12.1).
static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int lx, ly, errs;
    static bool was;
    static uint32_t last_log;
    if (forget) { forget = false; was = false; errs = 0; }
    uint32_t now = lv_tick_get();
    if (now - last_log > 15000) {
        ESP_LOGI(TAG, "reads ok=%d err=%d, up answers: no finger %d, other status %d", n_ok, n_err, n_nofinger,
                 n_status);
        last_log = now;
    }
    bool before = was;
    int x, y;
    int r = read_chip(&x, &y);
    if (r > 0) last_down_us = esp_timer_get_time();
    // The CST9217 NACKs mid-swipe now and then: hold the last point for a few reads. More than 5 errors in a row
    // count as a release (it NACKs instead of reporting "up"): a loop waiting for a clean "up" never ends.
    if (r < 0 && was && ++errs < 6) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = lx; data->point.y = ly;
    } else {
        errs = 0;
        if (r > 0) {
            lx = x; ly = y;
            if (!was) ESP_LOGI(TAG, "down %d,%d", x, y);
            was = true;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            if (was) ESP_LOGI(TAG, "up %d,%d", lx, ly);
            was = false;
            data->state = LV_INDEV_STATE_RELEASED;
        }
        data->point.x = lx;
        data->point.y = ly;
    }
    // A press the app's filter refused (a touch that wakes a dark screen) is no press for LVGL or the read hook, to the
    // finger's lift: no tap, no drag, no long-press. touch_get() still reports it (touch_idle_ms counts it).
    static bool swallow;
    if (data->state == LV_INDEV_STATE_PRESSED && !before && press_filter && press_filter()) swallow = true;
    if (swallow) {
        if (data->state == LV_INDEV_STATE_RELEASED) swallow = false;
        else data->state = LV_INDEV_STATE_RELEASED;
    }
    if (read_hook) read_hook(indev, data);               // before LVGL handles this read
}

void touch_register_lvgl(void)
{
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, read_cb);
}
