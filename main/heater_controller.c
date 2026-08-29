#include "heater_controller.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heater_policy.h"
#include "nvs.h"
#include "sdkconfig.h"

#define NVS_NAMESPACE "heater"
#define NVS_SETPOINT_KEY "setpoint_x10"
#define NVS_SETPOINT_PERSIST_KEY "persist_target"
#define NVS_MODE_KEY "saved_mode"
#define NVS_MODE_PERSIST_KEY "persist_mode"
#define CONTROLLER_PERIOD_MS 100U

#ifdef CONFIG_KHEATER_SETPOINT_PERSIST_DEFAULT
#define SETPOINT_PERSIST_DEFAULT true
#else
#define SETPOINT_PERSIST_DEFAULT false
#endif

#ifdef CONFIG_KHEATER_MODE_PERSIST_DEFAULT
#define MODE_PERSIST_DEFAULT true
#else
#define MODE_PERSIST_DEFAULT false
#endif

typedef struct {
	heater_mode_t mode;
	heater_stop_reason_t stop_reason;
	heater_output_state_t outputs;
	bool auto_enabled;
	bool temperature_valid;
	bool cooldown_active;
	bool setpoint_persistence_enabled;
	bool mode_persistence_enabled;
	float setpoint_c;
	float temperature_c;
	uint64_t temperature_ms;
	uint64_t cooldown_until_ms;
	uint64_t manual_until_ms;
	uint32_t manual_timeout_count;
	esp_err_t last_error;
	bool ready;
} controller_state_t;

static const char *TAG = "heater_ctrl";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static controller_state_t s_state;

static uint64_t now_ms(void)
{
	return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static float config_x10_to_float(int value)
{
	return (float)value / 10.0f;
}

static bool temperature_in_range(float value)
{
	return heater_policy_value_in_range(
		value, config_x10_to_float(CONFIG_KHEATER_TEMP_MIN_X10),
		config_x10_to_float(CONFIG_KHEATER_TEMP_MAX_X10));
}

static bool setpoint_in_range(float value)
{
	return heater_policy_value_in_range(
		value, config_x10_to_float(CONFIG_KHEATER_SETPOINT_MIN_X10),
		config_x10_to_float(CONFIG_KHEATER_SETPOINT_MAX_X10));
}

static void apply_outputs_locked(bool fan, bool low, bool high, bool rotation)
{
	heater_output_state_t next = {
		.fan = fan,
		.heat_low = low,
		.heat_high = high,
		.rotation = rotation,
	};
	esp_err_t err = heater_output_apply(&next);
	if (err == ESP_OK) {
		s_state.outputs = next;
	} else {
		s_state.last_error = err;
	}
}

static void begin_cooldown_locked(heater_stop_reason_t reason, uint64_t now)
{
	s_state.mode = s_state.auto_enabled ? HEATER_MODE_AUTO : HEATER_MODE_OFF;
	s_state.stop_reason = reason;
	s_state.cooldown_active = true;
	s_state.cooldown_until_ms = now + CONFIG_KHEATER_COOLDOWN_MS;
	s_state.manual_until_ms = 0;
	apply_outputs_locked(true, false, false, false);
}

static void apply_manual_locked(heater_mode_t mode, uint64_t now)
{
	s_state.auto_enabled = false;
	s_state.temperature_valid = false;
	s_state.cooldown_active = false;
	s_state.cooldown_until_ms = 0;
	s_state.stop_reason = HEATER_STOP_NONE;
	s_state.mode = mode;

	heater_policy_outputs_t outputs =
		heater_policy_manual_outputs((int)mode, s_state.outputs.rotation);
	s_state.manual_until_ms = (outputs.heat_low || outputs.heat_high)
		? now + CONFIG_KHEATER_MANUAL_HEAT_LIMIT_MS : 0;
	apply_outputs_locked(outputs.fan, outputs.heat_low, outputs.heat_high,
			     outputs.rotation);
}

static void evaluate_auto_locked(uint64_t now)
{
	if (!s_state.auto_enabled || !s_state.temperature_valid) {
		s_state.mode = HEATER_MODE_AUTO;
		apply_outputs_locked(false, false, false, false);
		return;
	}
	if (s_state.cooldown_active) return;

	float high_delta = config_x10_to_float(CONFIG_KHEATER_HIGH_DELTA_X10);
	heater_policy_outputs_t outputs = heater_policy_auto_outputs(
		s_state.setpoint_c, s_state.temperature_c, high_delta,
		s_state.outputs.rotation);
	s_state.mode = HEATER_MODE_AUTO;
	s_state.stop_reason = HEATER_STOP_NONE;
	apply_outputs_locked(outputs.fan, outputs.heat_low, outputs.heat_high,
			     outputs.rotation);
	(void)now;
}

typedef struct {
	float setpoint_c;
	bool setpoint_persistence_enabled;
	heater_mode_t mode;
	bool mode_persistence_enabled;
} controller_preferences_t;

static controller_preferences_t load_controller_preferences(void)
{
	controller_preferences_t preferences = {
		.setpoint_c = config_x10_to_float(CONFIG_KHEATER_SETPOINT_DEFAULT_X10),
		.setpoint_persistence_enabled = SETPOINT_PERSIST_DEFAULT,
		.mode = HEATER_MODE_OFF,
		.mode_persistence_enabled = MODE_PERSIST_DEFAULT,
	};
	int32_t x10 = CONFIG_KHEATER_SETPOINT_DEFAULT_X10;
	nvs_handle_t handle;
	if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
		uint8_t persist = preferences.setpoint_persistence_enabled ? 1U : 0U;
		esp_err_t persist_err = nvs_get_u8(handle, NVS_SETPOINT_PERSIST_KEY,
						   &persist);
		if (persist_err == ESP_OK) {
			preferences.setpoint_persistence_enabled = persist != 0;
		}
		if (preferences.setpoint_persistence_enabled) {
			int32_t stored = x10;
			if (nvs_get_i32(handle, NVS_SETPOINT_KEY, &stored) == ESP_OK) {
				float value = config_x10_to_float(stored);
				if (setpoint_in_range(value)) x10 = stored;
			}
		}
		uint8_t mode_persist = preferences.mode_persistence_enabled ? 1U : 0U;
		if (nvs_get_u8(handle, NVS_MODE_PERSIST_KEY, &mode_persist) == ESP_OK) {
			preferences.mode_persistence_enabled = mode_persist != 0;
		}
		if (preferences.mode_persistence_enabled) {
			uint8_t stored_mode = HEATER_MODE_OFF;
			if (nvs_get_u8(handle, NVS_MODE_KEY, &stored_mode) == ESP_OK &&
			    stored_mode <= HEATER_MODE_AUTO) {
				preferences.mode = (heater_mode_t)stored_mode;
			}
		}
		nvs_close(handle);
	}
	preferences.setpoint_c = config_x10_to_float(x10);
	return preferences;
}

static esp_err_t save_mode_preferences(bool enabled, heater_mode_t mode)
{
	if (mode > HEATER_MODE_AUTO) return ESP_ERR_INVALID_ARG;
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) return err;
	err = nvs_set_u8(handle, NVS_MODE_PERSIST_KEY, enabled ? 1U : 0U);
	if (err == ESP_OK && enabled) {
		err = nvs_set_u8(handle, NVS_MODE_KEY, (uint8_t)mode);
	} else if (err == ESP_OK) {
		esp_err_t erase_err = nvs_erase_key(handle, NVS_MODE_KEY);
		if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) err = erase_err;
	}
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

static esp_err_t persist_mode_locked(heater_mode_t mode)
{
	if (!s_state.mode_persistence_enabled) return ESP_OK;
	esp_err_t err = save_mode_preferences(true, mode);
	if (err != ESP_OK) s_state.last_error = err;
	return err;
}

static esp_err_t save_setpoint(float value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) return err;
	int32_t x10 = (int32_t)lroundf(value * 10.0f);
	err = nvs_set_i32(handle, NVS_SETPOINT_KEY, x10);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

static esp_err_t save_setpoint_preferences(bool enabled, float setpoint_c)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) return err;
	err = nvs_set_u8(handle, NVS_SETPOINT_PERSIST_KEY, enabled ? 1U : 0U);
	if (err == ESP_OK && enabled) {
		int32_t x10 = (int32_t)lroundf(setpoint_c * 10.0f);
		err = nvs_set_i32(handle, NVS_SETPOINT_KEY, x10);
	} else if (err == ESP_OK) {
		esp_err_t erase_err = nvs_erase_key(handle, NVS_SETPOINT_KEY);
		if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) err = erase_err;
	}
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

static void controller_task(void *arg)
{
	(void)arg;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(CONTROLLER_PERIOD_MS));
		uint64_t now = now_ms();
		if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) continue;

		if (s_state.cooldown_active &&
		    heater_policy_deadline_reached(now, s_state.cooldown_until_ms)) {
			s_state.cooldown_active = false;
			s_state.cooldown_until_ms = 0;
			if (s_state.auto_enabled && s_state.temperature_valid &&
			    now - s_state.temperature_ms < CONFIG_KHEATER_AUTO_TEMP_TIMEOUT_MS) {
				evaluate_auto_locked(now);
			} else {
				apply_outputs_locked(false, false, false, false);
			}
		}

		if (s_state.auto_enabled && s_state.temperature_valid &&
		    now - s_state.temperature_ms >= CONFIG_KHEATER_AUTO_TEMP_TIMEOUT_MS) {
			s_state.temperature_valid = false;
			begin_cooldown_locked(HEATER_STOP_TEMP_STALE, now);
			ESP_LOGW(TAG, "AUTO temperature stale; heat disabled");
		}

		if (!s_state.auto_enabled &&
		    heater_policy_deadline_reached(now, s_state.manual_until_ms)) {
			s_state.manual_timeout_count++;
			begin_cooldown_locked(HEATER_STOP_MANUAL_TIMEOUT, now);
			ESP_LOGW(TAG, "manual heat limit reached; cooling down");
		}
		xSemaphoreGive(s_lock);
	}
}

esp_err_t heater_controller_init(void)
{
	if (s_task) return ESP_OK;
	if (!heater_policy_self_test()) {
		ESP_LOGE(TAG, "deterministic heater policy self-test failed");
		return ESP_FAIL;
	}
	if (!s_lock) {
		s_lock = xSemaphoreCreateMutex();
		if (!s_lock) return ESP_ERR_NO_MEM;
	}

	memset(&s_state, 0, sizeof(s_state));
	controller_preferences_t preferences = load_controller_preferences();
	s_state.mode = HEATER_MODE_OFF;
	s_state.stop_reason = HEATER_STOP_BOOT;
	s_state.setpoint_c = preferences.setpoint_c;
	s_state.setpoint_persistence_enabled = preferences.setpoint_persistence_enabled;
	s_state.mode_persistence_enabled = preferences.mode_persistence_enabled;
	apply_outputs_locked(false, false, false, false);

	if (xTaskCreate(controller_task, "heater_ctrl", 3072, NULL, 8, &s_task) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		vTaskDelete(s_task);
		s_task = NULL;
		apply_outputs_locked(false, false, false, false);
		return ESP_ERR_TIMEOUT;
	}
	s_state.ready = true;
	if (s_state.mode_persistence_enabled) {
		if (preferences.mode == HEATER_MODE_AUTO) {
			s_state.auto_enabled = true;
			s_state.mode = HEATER_MODE_AUTO;
			s_state.stop_reason = HEATER_STOP_NONE;
			/* AUTO stays de-energized until a fresh temperature arrives. */
			apply_outputs_locked(false, false, false, false);
		} else if (preferences.mode >= HEATER_MODE_FAN &&
			   preferences.mode <= HEATER_MODE_MAX) {
			apply_manual_locked(preferences.mode, now_ms());
		}
	}
	heater_mode_t restored_mode = s_state.mode;
	float restored_setpoint = s_state.setpoint_c;
	bool target_persist = s_state.setpoint_persistence_enabled;
	bool mode_persist = s_state.mode_persistence_enabled;
	xSemaphoreGive(s_lock);
	ESP_LOGI(TAG, "controller ready mode=%s setpoint=%.1f C target_persist=%u mode_persist=%u",
		 heater_controller_mode_name(restored_mode), restored_setpoint,
		 target_persist ? 1U : 0U, mode_persist ? 1U : 0U);
	return ESP_OK;
}

bool heater_controller_ready(void)
{
	bool ready = false;
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return false;
	ready = s_state.ready;
	xSemaphoreGive(s_lock);
	return ready;
}

esp_err_t heater_controller_set_manual(heater_mode_t mode)
{
	if (mode < HEATER_MODE_FAN || mode > HEATER_MODE_MAX) return ESP_ERR_INVALID_ARG;
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	esp_err_t persist_err = persist_mode_locked(mode);
	if (persist_err != ESP_OK) {
		xSemaphoreGive(s_lock);
		return persist_err;
	}
	apply_manual_locked(mode, now_ms());
	xSemaphoreGive(s_lock);
	return ESP_OK;
}

esp_err_t heater_controller_set_off(void)
{
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	s_state.auto_enabled = false;
	s_state.temperature_valid = false;
	begin_cooldown_locked(HEATER_STOP_COMMAND, now_ms());
	/* OFF is safety-critical even if the optional NVS update fails. */
	if (persist_mode_locked(HEATER_MODE_OFF) != ESP_OK) {
		ESP_LOGE(TAG, "failed to persist OFF mode: %s", esp_err_to_name(s_state.last_error));
	}
	xSemaphoreGive(s_lock);
	return ESP_OK;
}

esp_err_t heater_controller_enable_auto(void)
{
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	esp_err_t persist_err = persist_mode_locked(HEATER_MODE_AUTO);
	if (persist_err != ESP_OK) {
		xSemaphoreGive(s_lock);
		return persist_err;
	}
	s_state.auto_enabled = true;
	s_state.temperature_valid = false;
	s_state.cooldown_active = false;
	s_state.cooldown_until_ms = 0;
	s_state.manual_until_ms = 0;
	s_state.mode = HEATER_MODE_AUTO;
	s_state.stop_reason = HEATER_STOP_NONE;
	apply_outputs_locked(false, false, false, false);
	xSemaphoreGive(s_lock);
	return ESP_OK;
}

esp_err_t heater_controller_feed_temperature(float temperature_c)
{
	if (!temperature_in_range(temperature_c)) return heater_controller_reject_temperature();
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	s_state.temperature_c = temperature_c;
	s_state.temperature_ms = now_ms();
	s_state.temperature_valid = true;
	if (s_state.auto_enabled && !s_state.cooldown_active) {
		evaluate_auto_locked(s_state.temperature_ms);
	}
	xSemaphoreGive(s_lock);
	return ESP_OK;
}

esp_err_t heater_controller_reject_temperature(void)
{
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	if (s_state.auto_enabled) {
		s_state.temperature_valid = false;
		begin_cooldown_locked(HEATER_STOP_TEMP_INVALID, now_ms());
	}
	xSemaphoreGive(s_lock);
	return ESP_ERR_INVALID_ARG;
}

static esp_err_t set_setpoint(float setpoint_c, bool persist)
{
	if (!setpoint_in_range(setpoint_c)) return ESP_ERR_INVALID_ARG;
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	if (persist && s_state.setpoint_persistence_enabled) {
		esp_err_t save_err = save_setpoint(setpoint_c);
		if (save_err != ESP_OK) {
			s_state.last_error = save_err;
			xSemaphoreGive(s_lock);
			return save_err;
		}
	}
	s_state.setpoint_c = setpoint_c;
	if (s_state.auto_enabled && s_state.temperature_valid && !s_state.cooldown_active) {
		evaluate_auto_locked(now_ms());
	}
	xSemaphoreGive(s_lock);
	return ESP_OK;
}

esp_err_t heater_controller_set_setpoint(float setpoint_c)
{
	return set_setpoint(setpoint_c, true);
}

esp_err_t heater_controller_set_setpoint_runtime(float setpoint_c)
{
	return set_setpoint(setpoint_c, false);
}

esp_err_t heater_controller_set_setpoint_persistence(bool enabled)
{
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	esp_err_t err = save_setpoint_preferences(enabled, s_state.setpoint_c);
	if (err == ESP_OK) {
		s_state.setpoint_persistence_enabled = enabled;
	} else {
		s_state.last_error = err;
	}
	xSemaphoreGive(s_lock);
	return err;
}

esp_err_t heater_controller_set_mode_persistence(bool enabled)
{
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	esp_err_t err = save_mode_preferences(enabled, s_state.mode);
	if (err == ESP_OK) {
		s_state.mode_persistence_enabled = enabled;
	} else {
		s_state.last_error = err;
	}
	xSemaphoreGive(s_lock);
	return err;
}

static esp_err_t set_rotation_locked(bool enabled)
{
	if (s_state.outputs.rotation == enabled) return ESP_OK;
	if (s_state.cooldown_active) return ESP_ERR_INVALID_STATE;
	apply_outputs_locked(s_state.outputs.fan, s_state.outputs.heat_low,
			     s_state.outputs.heat_high, enabled);
	return ESP_OK;
}

esp_err_t heater_controller_set_rotation(bool enabled)
{
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	esp_err_t err = set_rotation_locked(enabled);
	xSemaphoreGive(s_lock);
	return err;
}

esp_err_t heater_controller_toggle_rotation(bool *enabled)
{
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}
	bool desired = !s_state.outputs.rotation;
	esp_err_t err = set_rotation_locked(desired);
	if (err == ESP_OK && enabled) *enabled = desired;
	xSemaphoreGive(s_lock);
	return err;
}

void heater_controller_get_status(heater_controller_status_t *status)
{
	if (!status) return;
	memset(status, 0, sizeof(*status));
	status->temperature_age_ms = UINT64_MAX;
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
		status->last_error = ESP_ERR_TIMEOUT;
		return;
	}
	uint64_t now = now_ms();
	status->mode = s_state.mode;
	status->stop_reason = s_state.stop_reason;
	status->outputs = s_state.outputs;
	status->auto_enabled = s_state.auto_enabled;
	status->temperature_valid = s_state.temperature_valid;
	status->cooldown_active = s_state.cooldown_active;
	status->setpoint_persistence_enabled = s_state.setpoint_persistence_enabled;
	status->mode_persistence_enabled = s_state.mode_persistence_enabled;
	status->setpoint_c = s_state.setpoint_c;
	status->temperature_c = s_state.temperature_c;
	status->temperature_age_ms = s_state.temperature_valid
		? now - s_state.temperature_ms : UINT64_MAX;
	status->cooldown_remaining_ms =
		s_state.cooldown_active && s_state.cooldown_until_ms > now
		? s_state.cooldown_until_ms - now : 0;
	status->manual_remaining_ms =
		s_state.manual_until_ms > now ? s_state.manual_until_ms - now : 0;
	status->manual_timeout_count = s_state.manual_timeout_count;
	status->last_error = s_state.last_error;
	xSemaphoreGive(s_lock);
}

const char *heater_controller_mode_name(heater_mode_t mode)
{
	switch (mode) {
	case HEATER_MODE_OFF: return "OFF";
	case HEATER_MODE_FAN: return "FAN";
	case HEATER_MODE_LOW: return "LOW";
	case HEATER_MODE_HIGH: return "HIGH";
	case HEATER_MODE_MAX: return "MAX";
	case HEATER_MODE_AUTO: return "AUTO";
	default: return "?";
	}
}

const char *heater_controller_stop_reason_name(heater_stop_reason_t reason)
{
	switch (reason) {
	case HEATER_STOP_NONE: return "none";
	case HEATER_STOP_COMMAND: return "command";
	case HEATER_STOP_TEMP_STALE: return "temp_stale";
	case HEATER_STOP_TEMP_INVALID: return "temp_invalid";
	case HEATER_STOP_MANUAL_TIMEOUT: return "manual_timeout";
	case HEATER_STOP_BOOT: return "boot";
	default: return "unknown";
	}
}
