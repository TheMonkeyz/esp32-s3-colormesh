#pragma once
// Browser emulator: the USB console forge_lvgl's screens.c writes a "shot" to (no USB here: the bytes are dropped)
#include <stddef.h>
#include "freertos/FreeRTOS.h"
static inline int usb_serial_jtag_write_bytes(const void *src, size_t size, TickType_t wait) { (void)src; (void)wait; return (int)size; }
