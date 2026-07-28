#include "heater_policy.h"

#include <math.h>

bool heater_policy_value_in_range(float value, float minimum, float maximum)
{
	return isfinite(value) && value >= minimum && value <= maximum;
}

heater_policy_outputs_t heater_policy_manual_outputs(int mode, bool rotation)
{
	heater_policy_outputs_t outputs = {
		.fan = mode != 0,
		.heat_low = mode == 2 || mode == 4,
		.heat_high = mode == 3 || mode == 4,
		.rotation = mode != 0 && rotation,
	};
	return outputs;
}

heater_policy_outputs_t heater_policy_auto_outputs(float setpoint_c,
						   float temperature_c,
						   float high_delta_c,
						   bool rotation)
{
	float difference = setpoint_c - temperature_c;
	heater_policy_outputs_t outputs = {
		.fan = true,
		.heat_low = difference > 0.0f && difference <= high_delta_c,
		.heat_high = difference > high_delta_c,
		.rotation = rotation,
	};
	return outputs;
}

bool heater_policy_deadline_reached(uint64_t now_ms, uint64_t deadline_ms)
{
	return deadline_ms != 0 && now_ms >= deadline_ms;
}

static bool outputs_equal(heater_policy_outputs_t actual,
			  bool fan, bool low, bool high, bool rotation)
{
	return actual.fan == fan && actual.heat_low == low &&
	       actual.heat_high == high && actual.rotation == rotation;
}

bool heater_policy_self_test(void)
{
	if (!outputs_equal(heater_policy_manual_outputs(0, true),
			   false, false, false, false)) return false;
	if (!outputs_equal(heater_policy_manual_outputs(1, true),
			   true, false, false, true)) return false;
	if (!outputs_equal(heater_policy_manual_outputs(2, false),
			   true, true, false, false)) return false;
	if (!outputs_equal(heater_policy_manual_outputs(3, false),
			   true, false, true, false)) return false;
	if (!outputs_equal(heater_policy_manual_outputs(4, true),
			   true, true, true, true)) return false;

	if (!outputs_equal(heater_policy_auto_outputs(20.0f, 20.1f, 0.5f, false),
			   true, false, false, false)) return false;
	if (!outputs_equal(heater_policy_auto_outputs(20.0f, 19.75f, 0.5f, true),
			   true, true, false, true)) return false;
	if (!outputs_equal(heater_policy_auto_outputs(20.0f, 19.5f, 0.5f, false),
			   true, true, false, false)) return false;
	if (!outputs_equal(heater_policy_auto_outputs(20.0f, 19.4f, 0.5f, false),
			   true, false, true, false)) return false;

	if (!heater_policy_value_in_range(5.0f, 5.0f, 35.0f)) return false;
	if (!heater_policy_value_in_range(35.0f, 5.0f, 35.0f)) return false;
	if (heater_policy_value_in_range(4.9f, 5.0f, 35.0f)) return false;
	if (heater_policy_value_in_range(NAN, -40.0f, 80.0f)) return false;
	if (heater_policy_value_in_range(INFINITY, -40.0f, 80.0f)) return false;
	if (heater_policy_deadline_reached(999, 1000)) return false;
	if (!heater_policy_deadline_reached(1000, 1000)) return false;
	if (heater_policy_deadline_reached(1000, 0)) return false;
	return true;
}
