#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

extern i2c_master_bus_handle_t s_i2c_bus;

esp_err_t i2c_bus_init(void);
