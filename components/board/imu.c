// QMI8658 accelerometer (see imu.h). Registers from the QMI8658 datasheet: WHO_AM_I 0x00 = 0x05, CTRL1 0x02
// (bit 6 = register address auto-increment), CTRL2 0x03 (accelerometer range and data rate), CTRL7 0x08
// (bit 0 = accelerometer on), AX_L..AZ_H 0x35..0x3A (little-endian), RESET 0x60 (write 0xB0).
#include "imu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "imu";
static i2c_master_dev_handle_t dev;

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(dev, b, 2, 50);
}

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, n, 50);
}

bool imu_init(i2c_master_bus_handle_t bus)
{
    static const uint8_t addrs[] = { 0x6B, 0x6A };              // SA0 high / low
    for (int i = 0; i < 2 && !dev; i++) {
        if (!bus || i2c_master_probe(bus, addrs[i], 50) != ESP_OK) continue;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addrs[i],
                                   .scl_speed_hz = 400000 };
        if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) { dev = NULL; continue; }
        uint8_t id = 0;
        if (rd(0x00, &id, 1) != ESP_OK || id != 0x05) {
            ESP_LOGW(TAG, "device at 0x%02X is not a QMI8658 (id 0x%02X)", addrs[i], id);
            i2c_master_bus_rm_device(dev);
            dev = NULL;
            continue;
        }
        wr(0x60, 0xB0);                                          // soft reset
        vTaskDelay(pdMS_TO_TICKS(20));
        bool ok = wr(0x02, 0x40) == ESP_OK &&                    // address auto-increment, little-endian
                  wr(0x03, 0x07) == ESP_OK &&                    // ±2 g, 62.5 Hz
                  wr(0x08, 0x01) == ESP_OK;                      // accelerometer on, gyroscope off
        ESP_LOGI(TAG, "QMI8658 at 0x%02X %s", addrs[i], ok ? "ready" : "setup failed");
        if (!ok) { i2c_master_bus_rm_device(dev); dev = NULL; }
    }
    if (!dev) ESP_LOGW(TAG, "no motion sensor found");
    return dev != NULL;
}

bool imu_read(float g[3])
{
    uint8_t d[6];
    if (!dev || rd(0x35, d, 6) != ESP_OK) return false;
    for (int i = 0; i < 3; i++) g[i] = (int16_t)(d[2 * i] | d[2 * i + 1] << 8) / 16384.0f;   // ±2 g range
    return true;
}
