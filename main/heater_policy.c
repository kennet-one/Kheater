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

heater_policy_outputs_t heater_policy_auto_protected(float target, float temperature,
    float high_delta, float hysteresis, heater_policy_outputs_t current,
    uint64_t now, uint64_t changed, uint32_t interval, bool valid)
{
    heater_policy_outputs_t next = { .fan = true, .rotation = current.rotation };
    if (!valid || !isfinite(target) || !isfinite(temperature)) {
        next.rotation = false;
        return next;
    }
    float difference = target - temperature;
    /* Never retain heat at/above target just to satisfy a relay dwell timer. */
    if (difference <= 0.0f) return next;
    bool heating = current.heat_low || current.heat_high;
    if (!heating && difference < hysteresis) return next;
    bool high = current.heat_high
        ? difference > high_delta - hysteresis
        : difference > high_delta + hysteresis;
    next.heat_high = high;
    next.heat_low = !high;
    if ((next.heat_low != current.heat_low || next.heat_high != current.heat_high) &&
        (now < changed || now - changed < interval)) {
        return current;
    }
    return next;
}

static bool outputs_equal(heater_policy_outputs_t actual,
			  bool fan, bool low, bool high, bool rotation)
{
	return actual.fan == fan && actual.heat_low == low &&
	       actual.heat_high == high && actual.rotation == rotation;
}

bool heater_policy_self_test(void)
{
    heater_policy_outputs_t off = { .fan = true };
    heater_policy_outputs_t low = { .fan = true, .heat_low = true };
    heater_policy_outputs_t high = { .fan = true, .heat_high = true };
    const uint32_t intervals[] = {10000, 30000, 60000};
    for (unsigned i = 0; i < sizeof(intervals) / sizeof(intervals[0]); ++i) {
        uint32_t interval = intervals[i];
        if (!outputs_equal(heater_policy_auto_protected(25, 24, .5f, .2f,
            off, 1000 + interval - 1, 1000, interval, true), true, false, false, false)) return false;
        if (!outputs_equal(heater_policy_auto_protected(25, 24, .5f, .2f,
            off, 1000 + interval, 1000, interval, true), true, false, true, false)) return false;
        if (!outputs_equal(heater_policy_auto_protected(25, 25.1f, .5f, .2f,
            high, 1001, 1000, interval, true), true, false, false, false)) return false;
        if (!outputs_equal(heater_policy_auto_protected(25, 24.9f, .5f, .2f,
            high, 1001, 1000, interval, true), true, false, true, false)) return false;
    }
    if (heater_policy_auto_protected(25, 24, .5f, .2f, off, 29999, 0, 30000, true).heat_high) return false;
    if (!heater_policy_auto_protected(25, 24, .5f, .2f, off, 30000, 0, 30000, true).heat_high) return false;
    if (heater_policy_auto_protected(25, 24.9f, .5f, .2f, off, 60000, 0, 30000, true).heat_low) return false;
    if (!heater_policy_auto_protected(25, 24.49f, .5f, .2f, low, 60000, 0, 30000, true).heat_low) return false;
    if (!heater_policy_auto_protected(25, 24.51f, .5f, .2f, high, 60000, 0, 30000, true).heat_high) return false;
    if (heater_policy_auto_protected(25, 25, .5f, .2f, high, 1001, 1000, 60000, true).heat_high) return false;
    if (heater_policy_auto_protected(25, 20, .5f, .2f, high, 1001, 1000, 60000, false).heat_high) return false;
    if (heater_policy_auto_protected(25, NAN, .5f, .2f, low, 60000, 0, 30000, true).heat_low) return false;
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
