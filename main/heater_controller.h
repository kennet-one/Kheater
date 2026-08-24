#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "heater_output.h"

typedef enum {
	HEATER_MODE_OFF = 0,
	HEATER_MODE_FAN,
	HEATER_MODE_LOW,
	HEATER_MODE_HIGH,
	HEATER_MODE_MAX,
	HEATER_MODE_AUTO,
} heater_mode_t;

typedef enum {
	HEATER_STOP_NONE = 0,
	HEATER_STOP_COMMAND,
	HEATER_STOP_TEMP_STALE,
	HEATER_STOP_TEMP_INVALID,
	HEATER_STOP_MANUAL_TIMEOUT,
	HEATER_STOP_BOOT,
} heater_stop_reason_t;

typedef struct {
	heater_mode_t mode;
	heater_stop_reason_t stop_reason;
	heater_output_state_t outputs;
	bool auto_enabled;
	bool temperature_valid;
	bool cooldown_active;
	bool setpoint_persistence_enabled;
	float setpoint_c;
	float temperature_c;
	uint64_t temperature_age_ms;
	uint64_t cooldown_remaining_ms;
	uint64_t manual_remaining_ms;
	uint32_t manual_timeout_count;
	esp_err_t last_error;
} heater_controller_status_t;

esp_err_t heater_controller_init(void);
bool heater_controller_ready(void);
esp_err_t heater_controller_set_manual(heater_mode_t mode);
esp_err_t heater_controller_set_off(void);
esp_err_t heater_controller_enable_auto(void);
esp_err_t heater_controller_feed_temperature(float temperature_c);
esp_err_t heater_controller_reject_temperature(void);
esp_err_t heater_controller_set_setpoint(float setpoint_c);
esp_err_t heater_controller_set_setpoint_runtime(float setpoint_c);
esp_err_t heater_controller_set_setpoint_persistence(bool enabled);
esp_err_t heater_controller_toggle_rotation(bool *enabled);
void heater_controller_get_status(heater_controller_status_t *status);
const char *heater_controller_mode_name(heater_mode_t mode);
const char *heater_controller_stop_reason_name(heater_stop_reason_t reason);
