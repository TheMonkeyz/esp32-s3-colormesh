#pragma once
// Browser emulator: the BOOT button main.c reads at start-up (never held here)
#include <stdint.h>
#include "esp_err.h"
typedef enum { GPIO_NUM_0 = 0 } gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1 } gpio_mode_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; int pull_up_en, pull_down_en, intr_type; } gpio_config_t;
static inline esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
static inline int gpio_get_level(gpio_num_t n) { (void)n; return 1; }    // released
