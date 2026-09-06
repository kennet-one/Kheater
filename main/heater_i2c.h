#pragma once
#include "driver/i2c_master.h"

/* App-lifetime bus; device owners must never delete the shared bus. */
esp_err_t heater_i2c_init(void);
esp_err_t heater_i2c_add(uint8_t address, i2c_master_dev_handle_t *device);
esp_err_t heater_i2c_remove(i2c_master_dev_handle_t device);
