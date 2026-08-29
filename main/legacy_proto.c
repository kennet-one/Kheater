#include "legacy_proto.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "heater_controller.h"
#include "heater_display.h"
#include "heater_schedule.h"
#include "legacy_root_sender.h"

#define SCHEDULE_TIME_SYNC_STALE_MS 60000U

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

static bool parse_hex_field(const char *text, size_t offset, size_t digits,
			    uint32_t *value)
{
	if (!text || !value || digits == 0 || digits > 8) return false;
	uint32_t parsed = 0;
	for (size_t i = 0; i < digits; i++) {
		char c = text[offset + i];
		uint8_t nibble;
		if (c >= '0' && c <= '9') nibble = (uint8_t)(c - '0');
		else if (c >= 'a' && c <= 'f') nibble = (uint8_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') nibble = (uint8_t)(c - 'A' + 10);
		else return false;
		parsed = (parsed << 4) | nibble;
	}
	*value = parsed;
	return true;
}

static void format_schedule_meta(char *out, size_t out_size,
				 const heater_schedule_status_t *status)
{
	uint8_t active = status->active_index == HEATER_SCHEDULE_NO_INDEX
		? 0x0fU : status->active_index;
	uint8_t next = status->next_index == HEATER_SCHEDULE_NO_INDEX
		? 0x0fU : status->next_index;
	snprintf(out, out_size, "S5M%08" PRIX32 "%X%u%u%u%X%X",
		 status->config.generation, (unsigned)status->config.count,
		 status->config.enabled ? 1U : 0U,
		 status->config.persistence_enabled ? 1U : 0U,
		 status->clock_valid ? 1U : 0U, (unsigned)active, (unsigned)next);
}

static void format_schedule_diagnostic(char *out, size_t out_size,
				       const heater_schedule_status_t *status)
{
	uint8_t flags = (status->clock_valid ? 1U : 0U) |
		(status->config.enabled ? 2U : 0U) |
		(status->config.persistence_enabled ? 4U : 0U) |
		(status->catch_up_pending ? 8U : 0U) |
		(status->last_apply_valid ? 16U : 0U) |
		(status->time_sync_age_ms > SCHEDULE_TIME_SYNC_STALE_MS ? 32U : 0U);
	uint8_t weekday = status->local_weekday <= 6U ? status->local_weekday : 0x0fU;
	uint16_t minute = status->local_minute < 1440U ? status->local_minute : 0x0fffU;
	uint8_t last = status->last_apply_valid && status->last_apply_index < 8U ?
		status->last_apply_index : 0x0fU;
	uint8_t kind = status->last_apply_valid ? status->last_apply_kind : 0U;
	uint32_t age_s = status->last_apply_valid ? status->last_apply_age_ms / 1000U :
		0xffffU;
	if (age_s > 0xffffU) age_s = 0xffffU;
	snprintf(out, out_size, "S5D%08" PRIX32 "%02X%X%03X%X%X%04X%04X",
		 status->config.generation, (unsigned)flags, (unsigned)weekday,
		 (unsigned)minute, (unsigned)last, (unsigned)kind,
		 (unsigned)((uint32_t)status->last_error & 0xffffU), (unsigned)age_s);
}

void legacy_format_schedule_meta_token(char *out, size_t out_size)
{
	heater_schedule_status_t status;
	heater_schedule_get_status(&status);
	format_schedule_meta(out, out_size, &status);
}

void legacy_format_schedule_diagnostic_token(char *out, size_t out_size)
{
	heater_schedule_status_t status;
	heater_schedule_get_status(&status);
	format_schedule_diagnostic(out, out_size, &status);
}

static void format_schedule_point(char *out, size_t out_size, uint32_t generation,
				  uint8_t index,
				  const heater_schedule_point_t *point)
{
	snprintf(out, out_size, "S5P%08" PRIX32 "%X%u%03X%03X%X%02X",
		 generation, (unsigned)index, point->enabled ? 1U : 0U,
		 (unsigned)point->minute_of_day, (unsigned)(uint16_t)point->target_x10,
		 (unsigned)point->action, (unsigned)point->days_mask);
}

static bool execute_schedule_command(const char *text,
				     kheater_command_result_t *result)
{
	size_t length = strlen(text);
	uint32_t generation = 0;
	if (length == 14 && strncmp(text, "S5B", 3) == 0) {
		uint32_t count = 0, enabled = 0, persist = 0;
		if (!parse_hex_field(text, 3, 8, &generation) ||
		    !parse_hex_field(text, 11, 1, &count) ||
		    !parse_hex_field(text, 12, 1, &enabled) ||
		    !parse_hex_field(text, 13, 1, &persist) ||
		    count > HEATER_SCHEDULE_MAX_POINTS || enabled > 1 || persist > 1) {
			result->error = ESP_ERR_INVALID_ARG;
		} else {
			result->error = heater_schedule_stage_begin(generation, (uint8_t)count,
							     enabled != 0, persist != 0);
		}
	} else if (length == 22 && strncmp(text, "S5P", 3) == 0) {
		uint32_t index = 0, enabled = 0, minute = 0, target = 0, action = 0, days = 0;
		if (!parse_hex_field(text, 3, 8, &generation) ||
		    !parse_hex_field(text, 11, 1, &index) ||
		    !parse_hex_field(text, 12, 1, &enabled) ||
		    !parse_hex_field(text, 13, 3, &minute) ||
		    !parse_hex_field(text, 16, 3, &target) ||
		    !parse_hex_field(text, 19, 1, &action) ||
		    !parse_hex_field(text, 20, 2, &days) || enabled > 1 ||
		    index >= HEATER_SCHEDULE_MAX_POINTS || target > INT16_MAX) {
			result->error = ESP_ERR_INVALID_ARG;
		} else {
			heater_schedule_point_t point = {
				.enabled = enabled != 0,
				.minute_of_day = (uint16_t)minute,
				.target_x10 = (int16_t)target,
				.action = (heater_schedule_action_t)action,
				.days_mask = (uint8_t)days,
			};
			result->error = heater_schedule_stage_point(generation, (uint8_t)index,
							     &point);
		}
	} else if (length == 11 && strncmp(text, "S5C", 3) == 0) {
		if (!parse_hex_field(text, 3, 8, &generation)) {
			result->error = ESP_ERR_INVALID_ARG;
		} else {
			result->error = heater_schedule_stage_commit(generation);
			if (result->error == ESP_OK) {
				heater_schedule_status_t status;
				heater_schedule_get_status(&status);
				char reply[KHEATER_LEGACY_REPLY_LEN];
				format_schedule_meta(reply, sizeof(reply), &status);
				add_reply(result, reply);
			}
		}
	} else if (length == 3 && strcmp(text, "S5D") == 0) {
		heater_schedule_status_t status;
		heater_schedule_get_status(&status);
		char reply[KHEATER_LEGACY_REPLY_LEN];
		format_schedule_diagnostic(reply, sizeof(reply), &status);
		add_reply(result, reply);
		snprintf(result->result, sizeof(result->result), "%s", reply);
		return true;
	} else if (length == 3 && strcmp(text, "S5Q") == 0) {
		heater_schedule_status_t status;
		heater_schedule_get_status(&status);
		char reply[KHEATER_LEGACY_REPLY_LEN];
		format_schedule_meta(reply, sizeof(reply), &status);
		add_reply(result, reply);
		snprintf(result->result, sizeof(result->result), "%s", reply);
		return true;
	} else if (length == 4 && strncmp(text, "S5Q", 3) == 0) {
		uint32_t index = 0;
		if (!parse_hex_field(text, 3, 1, &index)) {
			result->error = ESP_ERR_INVALID_ARG;
		} else {
			heater_schedule_status_t status;
			heater_schedule_get_status(&status);
			if (index >= status.config.count) {
				result->error = ESP_ERR_NOT_FOUND;
			} else {
				char reply[KHEATER_LEGACY_REPLY_LEN];
				format_schedule_point(reply, sizeof(reply), status.config.generation,
						      (uint8_t)index,
						      &status.config.points[index]);
				add_reply(result, reply);
				snprintf(result->result, sizeof(result->result), "%s", reply);
				return true;
			}
		}
	} else {
		return false;
	}

	if (result->error == ESP_OK) {
		snprintf(result->result, sizeof(result->result), "accepted");
	} else {
		snprintf(result->result, sizeof(result->result), "%s",
			 esp_err_to_name(result->error));
	}
	return true;
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
		 "mode=%s auto=%u target=%.1f target_persist=%u mode_persist=%u temp=%s fan=%u low=%u high=%u rot=%u "
		 "cooldown=%llus manual=%llus stop=%s timeouts=%lu",
		 heater_controller_mode_name(status->mode), status->auto_enabled ? 1U : 0U,
		 status->setpoint_c, status->setpoint_persistence_enabled ? 1U : 0U,
		 status->mode_persistence_enabled ? 1U : 0U,
		 temp, status->outputs.fan ? 1U : 0U,
		 status->outputs.heat_low ? 1U : 0U,
		 status->outputs.heat_high ? 1U : 0U,
		 status->outputs.rotation ? 1U : 0U,
		 (unsigned long long)(status->cooldown_remaining_ms / 1000ULL),
		 (unsigned long long)(status->manual_remaining_ms / 1000ULL),
		 heater_controller_stop_reason_name(status->stop_reason),
		 (unsigned long)status->manual_timeout_count);
}

static void format_status_token_from_snapshot(
	char *out, size_t out_size, const heater_controller_status_t *status)
{
	if (!out || out_size == 0 || !status) return;
	char temperature[8];
	if (status->temperature_valid) {
		snprintf(temperature, sizeof(temperature), "%ld",
			 (long)lroundf(status->temperature_c * 10.0f));
	} else {
		snprintf(temperature, sizeof(temperature), "?");
	}
	snprintf(out, out_size,
		 "H5m%ua%uf%ul%uh%ur%uv%uc%us%ut%sp%uq%u",
		 (unsigned)status->mode, status->auto_enabled ? 1U : 0U,
		 status->outputs.fan ? 1U : 0U,
		 status->outputs.heat_low ? 1U : 0U,
		 status->outputs.heat_high ? 1U : 0U,
		 status->outputs.rotation ? 1U : 0U,
		 status->temperature_valid ? 1U : 0U,
		 status->cooldown_active ? 1U : 0U,
		 (unsigned)status->stop_reason, temperature,
		 status->setpoint_persistence_enabled ? 1U : 0U,
		 status->mode_persistence_enabled ? 1U : 0U);
	out[out_size - 1] = '\0';
}

void legacy_format_status_token(char *out, size_t out_size)
{
	heater_controller_status_t status;
	heater_controller_get_status(&status);
	format_status_token_from_snapshot(out, out_size, &status);
}

void legacy_format_display_status_token(char *out, size_t out_size)
{
	heater_display_status_t status;
	heater_display_get_status(&status);
	if (!out || out_size == 0) return;
	snprintf(out, out_size, "D5S%u%u%u", status.available ? 1U : 0U,
		 status.enabled ? 1U : 0U,
		 status.persistence_enabled ? 1U : 0U);
	out[out_size - 1] = '\0';
}

static void add_display_status_reply(kheater_command_result_t *result)
{
	char reply[KHEATER_LEGACY_REPLY_LEN];
	legacy_format_display_status_token(reply, sizeof(reply));
	add_reply(result, reply);
}

static void add_status_reply(kheater_command_result_t *result,
			     const heater_controller_status_t *status)
{
	char reply[KHEATER_LEGACY_REPLY_LEN];
	format_status_token_from_snapshot(reply, sizeof(reply), status);
	add_reply(result, reply);
}

bool legacy_execute_command(const char *text, kheater_command_result_t *result)
{
	if (!text || !text[0] || !result) return false;
	memset(result, 0, sizeof(*result));
	result->error = ESP_OK;

	heater_controller_status_t status;
	heater_controller_get_status(&status);
	if (execute_schedule_command(text, result)) return true;

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
	} else if (strlen(text) == 3 && text[0] == 'H' && text[1] == 'R' &&
		   (text[2] == '0' || text[2] == '1')) {
		result->error = heater_controller_set_rotation(text[2] == '1');
		heater_controller_get_status(&status);
		char reply[KHEATER_LEGACY_REPLY_LEN];
		snprintf(reply, sizeof(reply), "09%u",
			 status.outputs.rotation ? 1U : 0U);
		add_reply(result, reply);
		snprintf(result->result, sizeof(result->result), "rotation=%u",
			 status.outputs.rotation ? 1U : 0U);
		if (result->error == ESP_OK) add_status_reply(result, &status);
		return true;
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
		if (result->error == ESP_OK) add_status_reply(result, &status);
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
		add_status_reply(result, &status);
		add_display_status_reply(result);
		format_status(result->result, sizeof(result->result), &status);
		return true;
	} else if (strcmp(text, "heater.status") == 0) {
		heater_controller_get_status(&status);
		add_status_reply(result, &status);
		add_display_status_reply(result);
		format_status(result->result, sizeof(result->result), &status);
		return true;
	} else if (strlen(text) == 3 && text[0] == 'D' && text[1] == '5' &&
		   (text[2] == '0' || text[2] == '1')) {
		result->error = heater_display_set_enabled(text[2] == '1');
		add_display_status_reply(result);
		snprintf(result->result, sizeof(result->result), "display=%s",
			 text[2] == '1' ? "on" : "off");
		return true;
	} else if (strlen(text) == 4 && strncmp(text, "D5P", 3) == 0 &&
		   (text[3] == '0' || text[3] == '1')) {
		result->error = heater_display_set_persistence(text[3] == '1');
		add_display_status_reply(result);
		snprintf(result->result, sizeof(result->result), "display persistence=%s",
			 text[3] == '1' ? "on" : "off");
		return true;
	} else if (strcmp(text, "D5Q") == 0) {
		add_display_status_reply(result);
		snprintf(result->result, sizeof(result->result), "display status");
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
	} else if (strlen(text) == 3 && text[0] == 'P' && text[1] == '5' &&
		   (text[2] == '0' || text[2] == '1')) {
		result->error = heater_controller_set_setpoint_persistence(text[2] == '1');
	} else if (strlen(text) == 3 && text[0] == 'M' && text[1] == '5' &&
		   (text[2] == '0' || text[2] == '1')) {
		result->error = heater_controller_set_mode_persistence(text[2] == '1');
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
		if (result->error == ESP_OK) add_status_reply(result, &status);
		return true;
	} else {
		return false;
	}

	heater_controller_get_status(&status);
	if (result->error == ESP_OK) {
		add_mode_reply(result, &status);
		add_status_reply(result, &status);
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
