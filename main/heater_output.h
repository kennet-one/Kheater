#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
	bool fan;
	bool heat_low;
	bool heat_high;
	bool rotation;
} heater_output_state_t;

esp_err_t heater_output_init_safe(void);
esp_err_t heater_output_apply(const heater_output_state_t *state);
void heater_output_get(heater_output_state_t *state);
