#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"

// QMI8658 6-axis motion sensor on the touch I2C bus; only the accelerometer is used (wake on pick-up).
bool imu_init(i2c_master_bus_handle_t bus);   // false if the sensor doesn't answer
bool imu_read(float g[3]);                    // acceleration in g (x, y, z)
