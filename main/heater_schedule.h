#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define HEATER_SCHEDULE_MAX_POINTS 8
#define HEATER_SCHEDULE_ALL_DAYS 0x7fU
#define HEATER_SCHEDULE_NO_INDEX 0xffU

typedef enum {
	HEATER_SCHEDULE_ACTION_UNCHANGED = 0,
	HEATER_SCHEDULE_ACTION_OFF,
	HEATER_SCHEDULE_ACTION_AUTO,
	HEATER_SCHEDULE_ACTION_FAN,
	HEATER_SCHEDULE_ACTION_LOW,
	HEATER_SCHEDULE_ACTION_HIGH,
	HEATER_SCHEDULE_ACTION_MAX,
} heater_schedule_action_t;

typedef struct {
	bool enabled;
	uint16_t minute_of_day;
	int16_t target_x10;
	heater_schedule_action_t action;
	uint8_t days_mask;
} heater_schedule_point_t;

typedef struct {
	uint32_t generation;
	bool enabled;
	bool persistence_enabled;
	uint8_t count;
	heater_schedule_point_t points[HEATER_SCHEDULE_MAX_POINTS];
} heater_schedule_config_t;

typedef struct {
	heater_schedule_config_t config;
	bool clock_valid;
	uint8_t active_index;
	uint8_t next_index;
	uint16_t next_in_minutes;
	esp_err_t last_error;
} heater_schedule_status_t;

esp_err_t heater_schedule_start(void);
esp_err_t heater_schedule_stage_begin(uint32_t generation, uint8_t count,
				      bool enabled, bool persistence_enabled);
esp_err_t heater_schedule_stage_point(uint32_t generation, uint8_t index,
				      const heater_schedule_point_t *point);
esp_err_t heater_schedule_stage_commit(uint32_t generation);
void heater_schedule_get_status(heater_schedule_status_t *status);
bool heater_schedule_self_test(void);

