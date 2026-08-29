#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
	bool available;
	bool enabled;
	bool persistence_enabled;
	esp_err_t last_error;
} heater_display_status_t;

esp_err_t heater_display_start(void);
esp_err_t heater_display_set_enabled(bool enabled);
esp_err_t heater_display_set_persistence(bool enabled);
void heater_display_get_status(heater_display_status_t *status);
void heater_display_set_mesh_state(bool parent_connected, bool reliable_ready);
