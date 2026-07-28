#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	bool fan;
	bool heat_low;
	bool heat_high;
	bool rotation;
} heater_policy_outputs_t;

bool heater_policy_value_in_range(float value, float minimum, float maximum);
heater_policy_outputs_t heater_policy_manual_outputs(int mode, bool rotation);
heater_policy_outputs_t heater_policy_auto_outputs(float setpoint_c,
						   float temperature_c,
						   float high_delta_c,
						   bool rotation);
bool heater_policy_deadline_reached(uint64_t now_ms, uint64_t deadline_ms);
bool heater_policy_self_test(void);
