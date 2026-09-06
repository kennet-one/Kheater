#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "heater_climate_policy.h"

typedef struct {
    climate_policy_t policy;
    climate_source_t source;
    uint64_t now_ms;
    esp_err_t sensor_error;
} heater_climate_status_t;

esp_err_t heater_climate_start(void);
void heater_climate_get(heater_climate_status_t *out);
void heater_climate_format(char *out, size_t size);
/* Internal commands are accepted only by the authenticated root CONTROL hook. */
bool heater_climate_command(const char *text, esp_err_t *error);
