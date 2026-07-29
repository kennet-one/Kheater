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
	uint64_t last_sent_ms = 0;
	uint64_t last_attempt_ms = 0;

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
