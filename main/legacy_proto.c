#include "legacy_proto.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "heater_controller.h"
#include "legacy_root_sender.h"

static const char *TAG = "legacy";

static bool parse_float_strict(const char *text, float *value)
{
	if (!text || !text[0] || !value) return false;
	errno = 0;
	char *end = NULL;
	float parsed = strtof(text, &end);
	if (errno != 0 || end == text || !isfinite(parsed)) return false;
	while (*end == ' ' || *end == '\t') end++;
	if (*end != '\0') return false;
	*value = parsed;
	return true;
}

static void add_reply(kheater_command_result_t *result, const char *reply)
{
	if (!result || !reply || result->reply_count >= KHEATER_LEGACY_REPLY_MAX) return;
	snprintf(result->replies[result->reply_count],
		 sizeof(result->replies[result->reply_count]), "%s", reply);
	result->reply_count++;
}

static void add_mode_reply(kheater_command_result_t *result,
			   const heater_controller_status_t *status)
{
	if (status->auto_enabled) {
		add_reply(result, "A5");
		return;
	}
	const char *reply = "254";
	switch (status->mode) {
	case HEATER_MODE_FAN: reply = "250"; break;
	case HEATER_MODE_LOW: reply = "251"; break;
	case HEATER_MODE_HIGH: reply = "252"; break;
	case HEATER_MODE_MAX: reply = "253"; break;
	case HEATER_MODE_OFF:
	default: reply = "254"; break;
	}
	add_reply(result, reply);
}

static void format_status(char *out, size_t out_size,
			  const heater_controller_status_t *status)
{
	char temp[24];
	if (status->temperature_valid) {
		snprintf(temp, sizeof(temp), "%.1f/%llus", status->temperature_c,
			 (unsigned long long)(status->temperature_age_ms / 1000ULL));
	} else {
		snprintf(temp, sizeof(temp), "?");
	}
	snprintf(out, out_size,
		 "mode=%s auto=%u target=%.1f temp=%s fan=%u low=%u high=%u rot=%u "
		 "cooldown=%llus manual=%llus stop=%s timeouts=%lu",
		 heater_controller_mode_name(status->mode), status->auto_enabled ? 1U : 0U,
		 status->setpoint_c, temp, status->outputs.fan ? 1U : 0U,
		 status->outputs.heat_low ? 1U : 0U,
		 status->outputs.heat_high ? 1U : 0U,
		 status->outputs.rotation ? 1U : 0U,
		 (unsigned long long)(status->cooldown_remaining_ms / 1000ULL),
		 (unsigned long long)(status->manual_remaining_ms / 1000ULL),
		 heater_controller_stop_reason_name(status->stop_reason),
		 (unsigned long)status->manual_timeout_count);
}

bool legacy_execute_command(const char *text, kheater_command_result_t *result)
{
	if (!text || !text[0] || !result) return false;
	memset(result, 0, sizeof(*result));
	result->error = ESP_OK;

	heater_controller_status_t status;
	heater_controller_get_status(&status);

	if (strcmp(text, "he0") == 0) {
		result->error = heater_controller_set_manual(HEATER_MODE_FAN);
	} else if (strcmp(text, "he1") == 0) {
		result->error = heater_controller_set_manual(HEATER_MODE_LOW);
	} else if (strcmp(text, "he2") == 0) {
		result->error = heater_controller_set_manual(HEATER_MODE_HIGH);
	} else if (strcmp(text, "he3") == 0) {
		result->error = heater_controller_set_manual(HEATER_MODE_MAX);
	} else if (strcmp(text, "he4") == 0) {
		result->error = heater_controller_set_off();
	} else if (strcmp(text, "he5") == 0) {
		result->error = heater_controller_enable_auto();
	} else if (strcmp(text, "hero") == 0) {
		bool enabled = false;
		result->error = heater_controller_toggle_rotation(&enabled);
		heater_controller_get_status(&status);
		char reply[KHEATER_LEGACY_REPLY_LEN];
		snprintf(reply, sizeof(reply), "09%u",
			 status.outputs.rotation ? 1U : 0U);
		add_reply(result, reply);
		snprintf(result->result, sizeof(result->result), "rotation=%u",
			 status.outputs.rotation ? 1U : 0U);
		return true;
	} else if (strcmp(text, "heho") == 0) {
		heater_controller_get_status(&status);
		add_mode_reply(result, &status);
		char reply[KHEATER_LEGACY_REPLY_LEN];
		snprintf(reply, sizeof(reply), "09%u",
			 status.outputs.rotation ? 1U : 0U);
		add_reply(result, reply);
		snprintf(reply, sizeof(reply), "R5%.1f", status.setpoint_c);
		add_reply(result, reply);
		format_status(result->result, sizeof(result->result), &status);
		return true;
	} else if (strcmp(text, "heater.status") == 0) {
		heater_controller_get_status(&status);
		format_status(result->result, sizeof(result->result), &status);
		return true;
	} else if (strncmp(text, "W5", 2) == 0) {
		float setpoint = 0.0f;
		if (!parse_float_strict(text + 2, &setpoint)) {
			result->error = ESP_ERR_INVALID_ARG;
		} else {
			result->error = heater_controller_set_setpoint(setpoint);
		}
		if (result->error == ESP_OK) {
			heater_controller_get_status(&status);
			char reply[KHEATER_LEGACY_REPLY_LEN];
			snprintf(reply, sizeof(reply), "R5%.1f", status.setpoint_c);
			add_reply(result, reply);
		}
	} else if (strncmp(text, "05", 2) == 0) {
		float temperature = 0.0f;
		if (!parse_float_strict(text + 2, &temperature)) {
			result->error = heater_controller_reject_temperature();
		} else {
			result->error = heater_controller_feed_temperature(temperature);
		}
		heater_controller_get_status(&status);
		if (result->error == ESP_OK && status.auto_enabled) {
			add_reply(result, "A5");
		}
		snprintf(result->result, sizeof(result->result),
			 result->error == ESP_OK
			 ? (status.auto_enabled ? "AUTO temperature accepted"
						: "temperature ignored; AUTO disabled")
			 : "invalid external temperature");
		return true;
	} else {
		return false;
	}

	heater_controller_get_status(&status);
	if (result->error == ESP_OK) {
		add_mode_reply(result, &status);
		format_status(result->result, sizeof(result->result), &status);
	} else {
		snprintf(result->result, sizeof(result->result), "%s",
			 esp_err_to_name(result->error));
	}
	return true;
}

bool legacy_handle_command(const char *text)
{
	kheater_command_result_t result;
	if (!legacy_execute_command(text, &result)) return false;
	for (size_t i = 0; i < result.reply_count; i++) {
		(void)legacy_send_to_root(result.replies[i]);
	}
	return result.error == ESP_OK;
}

void legacy_handle_text(const char *text)
{
	if (!text || !text[0]) return;
	ESP_LOGI(TAG, "RX: \"%s\"", text);
	kheater_command_result_t result;
	if (!legacy_execute_command(text, &result)) {
		ESP_LOGD(TAG, "unsupported command: \"%s\"", text);
		return;
	}
	for (size_t i = 0; i < result.reply_count; i++) {
		(void)legacy_send_to_root(result.replies[i]);
	}
	if (result.error != ESP_OK) {
		ESP_LOGW(TAG, "command \"%s\" rejected: %s", text, result.result);
	}
}
