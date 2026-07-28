#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t heater_display_start(void);
void heater_display_set_mesh_state(bool parent_connected, bool reliable_ready);
