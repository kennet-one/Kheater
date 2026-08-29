#include "heater_status_publisher.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keemash_mesh_node.h"
#include "legacy_proto.h"
#include "legacy_root_sender.h"

#define STATUS_POLL_MS 500U
#define STATUS_HEARTBEAT_MS 15000U
#define STATUS_RETRY_MS 5000U
#define SCHEDULE_HEARTBEAT_MS 30000U

static const char *TAG = "heater_status";
static TaskHandle_t s_task;

static uint64_t now_ms(void)
{
	return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static void status_task(void *arg)
{
	(void)arg;
	char last_sent[KHEATER_LEGACY_REPLY_LEN] = {0};
	char last_schedule_meta[KHEATER_LEGACY_REPLY_LEN] = {0};
	char last_schedule_diag_key[KHEATER_LEGACY_REPLY_LEN] = {0};
	char last_display[KHEATER_LEGACY_REPLY_LEN] = {0};
	uint64_t last_sent_ms = 0;
	uint64_t last_attempt_ms = 0;
	uint64_t last_schedule_sent_ms = 0;
	uint64_t last_schedule_attempt_ms = 0;
	uint64_t last_display_sent_ms = 0;
	uint64_t last_display_attempt_ms = 0;

	for (;;) {
		char token[KHEATER_LEGACY_REPLY_LEN] = {0};
		legacy_format_status_token(token, sizeof(token));
		uint64_t now = now_ms();
		bool changed = strcmp(token, last_sent) != 0;
		bool heartbeat_due = now - last_sent_ms >= STATUS_HEARTBEAT_MS;
		bool retry_ready = now - last_attempt_ms >= STATUS_RETRY_MS;
		if ((changed || heartbeat_due) && retry_ready) {
			last_attempt_ms = now;
			esp_err_t err = mesh_v2_node_send_event(0, token);
			if (err != ESP_OK && legacy_send_to_root(token)) err = ESP_OK;
			if (err == ESP_OK) {
				snprintf(last_sent, sizeof(last_sent), "%s", token);
				last_sent_ms = now;
			} else {
				ESP_LOGD(TAG, "status submit deferred: %s",
					 esp_err_to_name(err));
			}
		}

		char schedule_meta[KHEATER_LEGACY_REPLY_LEN] = {0};
		char schedule_diag[KHEATER_LEGACY_REPLY_LEN] = {0};
		char schedule_diag_key[KHEATER_LEGACY_REPLY_LEN] = {0};
		legacy_format_schedule_meta_token(schedule_meta, sizeof(schedule_meta));
		legacy_format_schedule_diagnostic_token(schedule_diag, sizeof(schedule_diag));
		snprintf(schedule_diag_key, sizeof(schedule_diag_key), "%s", schedule_diag);
		size_t diag_length = strlen(schedule_diag_key);
		if (diag_length >= 4) memset(schedule_diag_key + diag_length - 4, '0', 4);
		bool schedule_changed = strcmp(schedule_meta, last_schedule_meta) != 0 ||
			strcmp(schedule_diag_key, last_schedule_diag_key) != 0;
		bool schedule_heartbeat = now - last_schedule_sent_ms >= SCHEDULE_HEARTBEAT_MS;
		bool schedule_retry = now - last_schedule_attempt_ms >= STATUS_RETRY_MS;
		if ((schedule_changed || schedule_heartbeat) && schedule_retry) {
			last_schedule_attempt_ms = now;
			bool ok = mesh_v2_node_send_event(0, schedule_meta) == ESP_OK &&
				mesh_v2_node_send_event(0, schedule_diag) == ESP_OK;
			if (ok) {
				snprintf(last_schedule_meta, sizeof(last_schedule_meta), "%s",
					 schedule_meta);
				snprintf(last_schedule_diag_key, sizeof(last_schedule_diag_key), "%s",
					 schedule_diag_key);
				last_schedule_sent_ms = now;
			}
		}

		char display_token[KHEATER_LEGACY_REPLY_LEN] = {0};
		legacy_format_display_status_token(display_token, sizeof(display_token));
		bool display_changed = strcmp(display_token, last_display) != 0;
		bool display_heartbeat = now - last_display_sent_ms >= STATUS_HEARTBEAT_MS;
		bool display_retry = now - last_display_attempt_ms >= STATUS_RETRY_MS;
		if ((display_changed || display_heartbeat) && display_retry) {
			last_display_attempt_ms = now;
			esp_err_t err = mesh_v2_node_send_event(0, display_token);
			if (err != ESP_OK && legacy_send_to_root(display_token)) err = ESP_OK;
			if (err == ESP_OK) {
				snprintf(last_display, sizeof(last_display), "%s", display_token);
				last_display_sent_ms = now;
			}
		}
		vTaskDelay(pdMS_TO_TICKS(STATUS_POLL_MS));
	}
}

esp_err_t heater_status_publisher_start(void)
{
	if (s_task) return ESP_OK;
	if (xTaskCreate(status_task, "heater_status", 3072, NULL, 5, &s_task) !=
	    pdPASS) {
		s_task = NULL;
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}
