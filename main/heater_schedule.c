#include "heater_schedule.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heater_controller.h"
#include "keemash_mesh_time.h"
#include "nvs.h"
#include "sdkconfig.h"

#define SCHEDULE_NVS_NAMESPACE "heater"
#define SCHEDULE_NVS_KEY "schedule_v1"
#define SCHEDULE_BLOB_MAGIC 0x3148534bUL
#define SCHEDULE_BLOB_VERSION 1U
#define SCHEDULE_TASK_PERIOD_MS 1000U
#define SCHEDULE_STAGE_TIMEOUT_MS 30000U
#define SCHEDULE_VALID_EPOCH 1577836800LL
#define SCHEDULE_FLAG_ENABLED 0x01U
#define SCHEDULE_FLAG_PERSIST 0x02U
#define POINT_FLAG_ENABLED 0x80U
#define CLOCK_CONTINUITY_LIMIT_SEC 120U
#define CLOCK_DRIFT_LIMIT_SEC 5U
#define APPLY_RETRY_MS 5000U

typedef struct __attribute__((packed)) {
	uint16_t minute_of_day;
	int16_t target_x10;
	uint8_t days_mask;
	uint8_t action_flags;
} persisted_point_t;

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint8_t version;
	uint8_t count;
	uint8_t flags;
	uint8_t reserved;
	uint32_t generation;
	persisted_point_t points[HEATER_SCHEDULE_MAX_POINTS];
	uint32_t checksum;
} schedule_blob_t;

typedef struct {
	bool active;
	uint32_t started_ms;
	uint8_t received_mask;
	heater_schedule_config_t config;
} schedule_stage_t;

static const char *TAG = "heater_sched";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static heater_schedule_config_t s_config;
static schedule_stage_t s_stage;
static uint32_t s_last_run_day[HEATER_SCHEDULE_MAX_POINTS];
static bool s_catchup_pending;
static bool s_last_clock_valid;
static time_t s_last_wall_epoch;
static uint32_t s_last_sample_ms;
static bool s_retry_pending;
static uint32_t s_retry_generation;
static uint32_t s_retry_day_key;
static uint32_t s_next_retry_ms;
static uint8_t s_retry_index;
static bool s_last_apply_valid;
static uint8_t s_last_apply_index = HEATER_SCHEDULE_NO_INDEX;
static uint8_t s_last_apply_kind = HEATER_SCHEDULE_APPLY_NONE;
static uint32_t s_last_apply_ms;
static esp_err_t s_last_error;

static uint32_t monotonic_ms(void)
{
	return (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
}

static uint32_t checksum32(const void *data, size_t length)
{
	const uint8_t *bytes = data;
	uint32_t value = 2166136261UL;
	for (size_t i = 0; i < length; i++) {
		value ^= bytes[i];
		value *= 16777619UL;
	}
	return value;
}

static bool action_valid(heater_schedule_action_t action)
{
	return action >= HEATER_SCHEDULE_ACTION_UNCHANGED &&
	       action <= HEATER_SCHEDULE_ACTION_MAX;
}

static bool point_valid(const heater_schedule_point_t *point)
{
	return point && point->minute_of_day < 24U * 60U &&
	       point->target_x10 >= CONFIG_KHEATER_SETPOINT_MIN_X10 &&
	       point->target_x10 <= CONFIG_KHEATER_SETPOINT_MAX_X10 &&
	       point->days_mask != 0 &&
	       (point->days_mask & ~HEATER_SCHEDULE_ALL_DAYS) == 0 &&
	       action_valid(point->action);
}

static bool config_valid(const heater_schedule_config_t *config)
{
	if (!config || config->generation == 0 ||
	    config->count > HEATER_SCHEDULE_MAX_POINTS) return false;
	for (uint8_t i = 0; i < config->count; i++) {
		if (!point_valid(&config->points[i])) return false;
		if (!config->points[i].enabled) continue;
		for (uint8_t j = 0; j < i; j++) {
			if (config->points[j].enabled &&
			    config->points[j].minute_of_day == config->points[i].minute_of_day &&
			    (config->points[j].days_mask & config->points[i].days_mask) != 0) {
				return false;
			}
		}
	}
	return true;
}

static uint8_t weekday_index(const struct tm *local)
{
	return (uint8_t)((local->tm_wday + 6) % 7);
}

static bool point_applies_on(const heater_schedule_point_t *point, int weekday)
{
	return point->enabled && weekday >= 0 && weekday < 7 &&
	       (point->days_mask & (1U << weekday)) != 0;
}

static uint8_t latest_point_index(const heater_schedule_config_t *config,
				  int weekday, uint16_t minute)
{
	uint32_t best_age = UINT_MAX;
	uint8_t best = HEATER_SCHEDULE_NO_INDEX;
	for (uint8_t day_back = 0; day_back < 7; day_back++) {
		int day = (weekday - day_back + 7) % 7;
		for (uint8_t i = 0; i < config->count; i++) {
			const heater_schedule_point_t *point = &config->points[i];
			if (!point_applies_on(point, day)) continue;
			if (day_back == 0 && point->minute_of_day > minute) continue;
			uint32_t age = (uint32_t)day_back * 1440U + minute -
				       point->minute_of_day;
			if (age < best_age) {
				best_age = age;
				best = i;
			}
		}
	}
	return best;
}

static uint8_t next_point_index(const heater_schedule_config_t *config,
				int weekday, uint16_t minute, uint16_t *distance)
{
	uint32_t best_distance = UINT_MAX;
	uint8_t best = HEATER_SCHEDULE_NO_INDEX;
	for (uint8_t day_forward = 0; day_forward < 7; day_forward++) {
		int day = (weekday + day_forward) % 7;
		for (uint8_t i = 0; i < config->count; i++) {
			const heater_schedule_point_t *point = &config->points[i];
			if (!point_applies_on(point, day)) continue;
			int32_t delta = (int32_t)day_forward * 1440 +
					(int32_t)point->minute_of_day - minute;
			if (delta <= 0) delta += 7 * 1440;
			if ((uint32_t)delta < best_distance) {
				best_distance = (uint32_t)delta;
				best = i;
			}
		}
	}
	if (distance) {
		*distance = best_distance == UINT_MAX ? UINT16_MAX :
			(uint16_t)(best_distance > UINT16_MAX ? UINT16_MAX : best_distance);
	}
	return best;
}

static uint32_t local_day_key(const struct tm *local)
{
	return ((uint32_t)(local->tm_year + 1900) << 9) | (uint32_t)local->tm_yday;
}

static bool clock_snapshot(struct tm *local, time_t *epoch)
{
	time_t now = time(NULL);
	if (now <= (time_t)SCHEDULE_VALID_EPOCH || localtime_r(&now, local) == NULL) {
		return false;
	}
	if (epoch) *epoch = now;
	return true;
}

static schedule_blob_t config_to_blob(const heater_schedule_config_t *config)
{
	schedule_blob_t blob = {
		.magic = SCHEDULE_BLOB_MAGIC,
		.version = SCHEDULE_BLOB_VERSION,
		.count = config->count,
		.flags = (config->enabled ? SCHEDULE_FLAG_ENABLED : 0U) |
			 (config->persistence_enabled ? SCHEDULE_FLAG_PERSIST : 0U),
		.generation = config->generation,
	};
	for (uint8_t i = 0; i < config->count; i++) {
		blob.points[i].minute_of_day = config->points[i].minute_of_day;
		blob.points[i].target_x10 = config->points[i].target_x10;
		blob.points[i].days_mask = config->points[i].days_mask;
		blob.points[i].action_flags = (uint8_t)config->points[i].action |
			(config->points[i].enabled ? POINT_FLAG_ENABLED : 0U);
	}
	blob.checksum = checksum32(&blob, offsetof(schedule_blob_t, checksum));
	return blob;
}

static heater_schedule_config_t blob_to_config(const schedule_blob_t *blob)
{
	heater_schedule_config_t config = {
		.generation = blob->generation,
		.enabled = (blob->flags & SCHEDULE_FLAG_ENABLED) != 0,
		.persistence_enabled = (blob->flags & SCHEDULE_FLAG_PERSIST) != 0,
		.count = blob->count,
	};
	for (uint8_t i = 0; i < config.count && i < HEATER_SCHEDULE_MAX_POINTS; i++) {
		config.points[i].minute_of_day = blob->points[i].minute_of_day;
		config.points[i].target_x10 = blob->points[i].target_x10;
		config.points[i].days_mask = blob->points[i].days_mask;
		config.points[i].enabled =
			(blob->points[i].action_flags & POINT_FLAG_ENABLED) != 0;
		config.points[i].action = (heater_schedule_action_t)
			(blob->points[i].action_flags & ~POINT_FLAG_ENABLED);
	}
	return config;
}

static esp_err_t persist_config(const heater_schedule_config_t *config)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) return err;
	if (config->persistence_enabled) {
		schedule_blob_t blob = config_to_blob(config);
		err = nvs_set_blob(handle, SCHEDULE_NVS_KEY, &blob, sizeof(blob));
	} else {
		err = nvs_erase_key(handle, SCHEDULE_NVS_KEY);
		if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
	}
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

static void load_config(void)
{
	memset(&s_config, 0, sizeof(s_config));
	s_config.generation = 1;
	nvs_handle_t handle;
	if (nvs_open(SCHEDULE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
	schedule_blob_t blob = {0};
	size_t size = sizeof(blob);
	esp_err_t err = nvs_get_blob(handle, SCHEDULE_NVS_KEY, &blob, &size);
	nvs_close(handle);
	if (err != ESP_OK || size != sizeof(blob) ||
	    blob.magic != SCHEDULE_BLOB_MAGIC || blob.version != SCHEDULE_BLOB_VERSION ||
	    blob.checksum != checksum32(&blob, offsetof(schedule_blob_t, checksum))) {
		if (err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "stored schedule rejected");
		return;
	}
	heater_schedule_config_t candidate = blob_to_config(&blob);
	if (!candidate.persistence_enabled || !config_valid(&candidate)) {
		ESP_LOGW(TAG, "stored schedule validation failed");
		return;
	}
	s_config = candidate;
}

static esp_err_t apply_action(const heater_schedule_point_t *point)
{
	esp_err_t setpoint_err =
		heater_controller_set_setpoint_runtime((float)point->target_x10 / 10.0f);
	esp_err_t action_err = ESP_OK;
	switch (point->action) {
	case HEATER_SCHEDULE_ACTION_UNCHANGED: break;
	case HEATER_SCHEDULE_ACTION_OFF: action_err = heater_controller_set_off(); break;
	case HEATER_SCHEDULE_ACTION_AUTO: action_err = heater_controller_enable_auto(); break;
	case HEATER_SCHEDULE_ACTION_FAN:
		action_err = heater_controller_set_manual(HEATER_MODE_FAN); break;
	case HEATER_SCHEDULE_ACTION_LOW:
		action_err = heater_controller_set_manual(HEATER_MODE_LOW); break;
	case HEATER_SCHEDULE_ACTION_HIGH:
		action_err = heater_controller_set_manual(HEATER_MODE_HIGH); break;
	case HEATER_SCHEDULE_ACTION_MAX:
		action_err = heater_controller_set_manual(HEATER_MODE_MAX); break;
	default: action_err = ESP_ERR_INVALID_ARG; break;
	}
	return setpoint_err != ESP_OK ? setpoint_err : action_err;
}

static void expire_stage_locked(uint32_t now)
{
	if (s_stage.active && now - s_stage.started_ms >= SCHEDULE_STAGE_TIMEOUT_MS) {
		memset(&s_stage, 0, sizeof(s_stage));
		ESP_LOGW(TAG, "staged schedule expired");
	}
}

static bool deadline_reached(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) >= 0;
}

static bool clock_is_continuous(time_t previous_epoch, time_t current_epoch,
				uint32_t previous_ms, uint32_t current_ms)
{
	int64_t wall_delta = (int64_t)current_epoch - (int64_t)previous_epoch;
	uint32_t monotonic_delta_ms = current_ms - previous_ms;
	int64_t monotonic_delta = (int64_t)((monotonic_delta_ms + 500U) / 1000U);
	int64_t drift = wall_delta - monotonic_delta;
	if (drift < 0) drift = -drift;
	return wall_delta >= 0 && wall_delta <= CLOCK_CONTINUITY_LIMIT_SEC &&
	       drift <= CLOCK_DRIFT_LIMIT_SEC;
}

static void record_apply_result(const heater_schedule_config_t *config,
				uint8_t index, bool catch_up, uint32_t day_key,
				esp_err_t error, uint32_t now)
{
	if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
	if (s_config.generation != config->generation) {
		xSemaphoreGive(s_lock);
		return;
	}
	s_last_error = error;
	if (error == ESP_OK) {
		s_last_apply_valid = true;
		s_last_apply_index = index;
		s_last_apply_kind = catch_up ? HEATER_SCHEDULE_APPLY_CATCH_UP :
			HEATER_SCHEDULE_APPLY_SCHEDULED;
		s_last_apply_ms = now;
		s_next_retry_ms = 0;
		if (catch_up) {
			s_catchup_pending = false;
		} else {
			s_last_run_day[index] = day_key;
			s_retry_pending = false;
		}
	} else {
		s_next_retry_ms = now + APPLY_RETRY_MS;
		if (!catch_up) {
			s_retry_pending = true;
			s_retry_generation = config->generation;
			s_retry_index = index;
			s_retry_day_key = day_key;
		}
	}
	xSemaphoreGive(s_lock);
}

static esp_err_t apply_schedule_point(const heater_schedule_config_t *config,
				      uint8_t index, bool catch_up,
				      uint32_t day_key, uint32_t now)
{
	esp_err_t err = catch_up ?
		heater_controller_set_setpoint_runtime(
			(float)config->points[index].target_x10 / 10.0f) :
		apply_action(&config->points[index]);
	record_apply_result(config, index, catch_up, day_key, err, now);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s point %u failed: %s",
			 catch_up ? "catch-up" : "scheduled", (unsigned)index,
			 esp_err_to_name(err));
	}
	return err;
}

static void run_scheduled_point(const heater_schedule_config_t *config,
				uint8_t index, uint32_t day_key, uint32_t now)
{
	bool execute = false;
	if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
		bool retry_wait = s_retry_pending &&
			s_retry_generation == config->generation &&
			s_retry_index == index && s_retry_day_key == day_key &&
			!deadline_reached(now, s_next_retry_ms);
		execute = s_config.generation == config->generation &&
			s_last_run_day[index] != day_key && !retry_wait;
		xSemaphoreGive(s_lock);
	}
	if (!execute) return;
	if (apply_schedule_point(config, index, false, day_key, now) == ESP_OK) {
		ESP_LOGI(TAG, "point %u applied target=%.1f action=%u", (unsigned)index,
			 (double)config->points[index].target_x10 / 10.0,
			 (unsigned)config->points[index].action);
	}
}

static void run_points_for_minute(const heater_schedule_config_t *config,
				  const struct tm *local, uint32_t now)
{
	uint8_t weekday = weekday_index(local);
	uint16_t minute = (uint16_t)(local->tm_hour * 60 + local->tm_min);
	uint32_t day_key = local_day_key(local);
	for (uint8_t i = 0; i < config->count; i++) {
		const heater_schedule_point_t *point = &config->points[i];
		if (point_applies_on(point, weekday) && point->minute_of_day == minute) {
			run_scheduled_point(config, i, day_key, now);
		}
	}
}

static void run_pending_retry(const heater_schedule_config_t *config, uint32_t now)
{
	bool execute = false;
	uint8_t index = HEATER_SCHEDULE_NO_INDEX;
	uint32_t day_key = 0;
	if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
		if (s_retry_pending && s_retry_generation == config->generation &&
		    s_retry_index < config->count && deadline_reached(now, s_next_retry_ms)) {
			execute = true;
			index = s_retry_index;
			day_key = s_retry_day_key;
		}
		xSemaphoreGive(s_lock);
	}
	if (execute) (void)apply_schedule_point(config, index, false, day_key, now);
}

static void schedule_task(void *arg)
{
	(void)arg;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(SCHEDULE_TASK_PERIOD_MS));
		struct tm local = {0};
		time_t epoch = 0;
		uint32_t now = monotonic_ms();
		bool clock_valid = clock_snapshot(&local, &epoch);
		heater_schedule_config_t config;
		bool catchup = false, continuous = false;
		time_t previous_epoch = 0;
		if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) continue;
		expire_stage_locked(now);
		config = s_config;
		if (!clock_valid) {
			s_last_clock_valid = false;
		} else {
			previous_epoch = s_last_wall_epoch;
			continuous = s_last_clock_valid &&
				clock_is_continuous(s_last_wall_epoch, epoch,
						    s_last_sample_ms, now);
			if (!s_last_clock_valid || !continuous) {
				s_catchup_pending = config.enabled;
				s_retry_pending = false;
				s_next_retry_ms = 0;
			}
			s_last_clock_valid = true;
			s_last_wall_epoch = epoch;
			s_last_sample_ms = now;
		}
		catchup = s_catchup_pending && config.enabled && clock_valid &&
			(s_next_retry_ms == 0 || deadline_reached(now, s_next_retry_ms));
		xSemaphoreGive(s_lock);
		if (!clock_valid || !config.enabled) continue;

		uint8_t weekday = weekday_index(&local);
		uint16_t minute = (uint16_t)(local.tm_hour * 60 + local.tm_min);
		if (catchup) {
			uint8_t latest = latest_point_index(&config, weekday, minute);
			if (latest != HEATER_SCHEDULE_NO_INDEX) {
				esp_err_t err = apply_schedule_point(&config, latest, true,
							local_day_key(&local), now);
				if (err == ESP_OK && config.points[latest].minute_of_day == minute &&
				    xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
					if (s_config.generation == config.generation) {
						s_last_run_day[latest] = local_day_key(&local);
					}
					xSemaphoreGive(s_lock);
				}
			} else if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
				if (s_config.generation == config.generation) {
					s_catchup_pending = false;
					s_next_retry_ms = 0;
				}
				xSemaphoreGive(s_lock);
			}
		}
		run_pending_retry(&config, now);

		if (continuous && previous_epoch < epoch) {
			time_t boundary = ((previous_epoch / 60) + 1) * 60;
			for (; boundary <= epoch; boundary += 60) {
				struct tm crossed = {0};
				if (localtime_r(&boundary, &crossed) != NULL) {
					run_points_for_minute(&config, &crossed, now);
				}
			}
		}
		run_points_for_minute(&config, &local, now);
	}
}

bool heater_schedule_self_test(void)
{
	heater_schedule_config_t config = {
		.generation = 7,
		.enabled = true,
		.count = 2,
		.points = {
			{ true, 360, 220, HEATER_SCHEDULE_ACTION_UNCHANGED, HEATER_SCHEDULE_ALL_DAYS },
			{ true, 1380, 180, HEATER_SCHEDULE_ACTION_OFF, HEATER_SCHEDULE_ALL_DAYS },
		},
	};
	if (!config_valid(&config) || latest_point_index(&config, 0, 400) != 0 ||
	    latest_point_index(&config, 0, 100) != 1) return false;
	uint16_t distance = 0;
	if (next_point_index(&config, 0, 400, &distance) != 1 || distance != 980) return false;
	config.points[1].minute_of_day = config.points[0].minute_of_day;
	if (config_valid(&config)) return false;
	config.points[1].days_mask = 1U << 1;
	config.points[0].days_mask = 1U << 0;
	if (!config_valid(&config) ||
	    !clock_is_continuous(1000, 1061, 1000, 62000) ||
	    clock_is_continuous(1000, 1121, 1000, 122000) ||
	    clock_is_continuous(1000, 995, 1000, 6000) ||
	    clock_is_continuous(1000, 1070, 1000, 62000)) return false;
	return true;
}

esp_err_t heater_schedule_start(void)
{
	if (s_task) return ESP_OK;
	if (!heater_schedule_self_test()) return ESP_FAIL;
	if (!s_lock) s_lock = xSemaphoreCreateMutex();
	if (!s_lock) return ESP_ERR_NO_MEM;
	load_config();
	for (size_t i = 0; i < HEATER_SCHEDULE_MAX_POINTS; i++) {
		s_last_run_day[i] = UINT32_MAX;
	}
	s_catchup_pending = s_config.enabled;
	s_retry_pending = false;
	s_next_retry_ms = 0;
	s_last_apply_valid = false;
	s_last_apply_index = HEATER_SCHEDULE_NO_INDEX;
	s_last_apply_kind = HEATER_SCHEDULE_APPLY_NONE;
	if (xTaskCreate(schedule_task, "heater_sched", 3584, NULL, 7, &s_task) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	ESP_LOGI(TAG, "ready enabled=%u persist=%u points=%u generation=%lu",
		 s_config.enabled ? 1U : 0U, s_config.persistence_enabled ? 1U : 0U,
		 (unsigned)s_config.count, (unsigned long)s_config.generation);
	return ESP_OK;
}

esp_err_t heater_schedule_stage_begin(uint32_t generation, uint8_t count,
				      bool enabled, bool persistence_enabled)
{
	if (!s_lock || generation == 0 || count > HEATER_SCHEDULE_MAX_POINTS) {
		return ESP_ERR_INVALID_ARG;
	}
	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
	memset(&s_stage, 0, sizeof(s_stage));
	s_stage.active = true;
	s_stage.started_ms = monotonic_ms();
	s_stage.config.generation = generation;
	s_stage.config.count = count;
	s_stage.config.enabled = enabled;
	s_stage.config.persistence_enabled = persistence_enabled;
	xSemaphoreGive(s_lock);
	return ESP_OK;
}

esp_err_t heater_schedule_stage_point(uint32_t generation, uint8_t index,
				      const heater_schedule_point_t *point)
{
	if (!s_lock || !point || !point_valid(point)) return ESP_ERR_INVALID_ARG;
	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
	expire_stage_locked(monotonic_ms());
	if (!s_stage.active || s_stage.config.generation != generation ||
	    index >= s_stage.config.count) {
		xSemaphoreGive(s_lock);
		return ESP_ERR_INVALID_STATE;
	}
	s_stage.config.points[index] = *point;
	s_stage.received_mask |= (uint8_t)(1U << index);
	xSemaphoreGive(s_lock);
	return ESP_OK;
}

esp_err_t heater_schedule_stage_commit(uint32_t generation)
{
	if (!s_lock) return ESP_ERR_INVALID_STATE;
	heater_schedule_config_t candidate;
	if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
	expire_stage_locked(monotonic_ms());
	uint8_t expected = s_stage.config.count == 0 ? 0U :
		(uint8_t)((1U << s_stage.config.count) - 1U);
	if (!s_stage.active || s_stage.config.generation != generation ||
	    s_stage.received_mask != expected || !config_valid(&s_stage.config)) {
		xSemaphoreGive(s_lock);
		return ESP_ERR_INVALID_STATE;
	}
	candidate = s_stage.config;
	esp_err_t err = persist_config(&candidate);
	if (err != ESP_OK) {
		s_last_error = err;
		xSemaphoreGive(s_lock);
		return err;
	}
	s_config = candidate;
	memset(&s_stage, 0, sizeof(s_stage));
	for (size_t i = 0; i < HEATER_SCHEDULE_MAX_POINTS; i++) {
		s_last_run_day[i] = UINT32_MAX;
	}
	s_catchup_pending = s_config.enabled;
	s_retry_pending = false;
	s_next_retry_ms = 0;
	s_last_apply_valid = false;
	s_last_apply_index = HEATER_SCHEDULE_NO_INDEX;
	s_last_apply_kind = HEATER_SCHEDULE_APPLY_NONE;
	s_last_error = ESP_OK;
	xSemaphoreGive(s_lock);
	return ESP_OK;
}

void heater_schedule_get_status(heater_schedule_status_t *status)
{
	if (!status) return;
	memset(status, 0, sizeof(*status));
	status->local_weekday = HEATER_SCHEDULE_NO_INDEX;
	status->local_minute = UINT16_MAX;
	status->active_index = HEATER_SCHEDULE_NO_INDEX;
	status->next_index = HEATER_SCHEDULE_NO_INDEX;
	status->next_in_minutes = UINT16_MAX;
	status->last_apply_index = HEATER_SCHEDULE_NO_INDEX;
	status->last_apply_kind = HEATER_SCHEDULE_APPLY_NONE;
	status->last_apply_age_ms = UINT32_MAX;
	status->time_sync_age_ms = UINT32_MAX;
	if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
		status->last_error = ESP_ERR_TIMEOUT;
		return;
	}
	status->config = s_config;
	status->catch_up_pending = s_catchup_pending;
	status->last_apply_valid = s_last_apply_valid;
	status->last_apply_index = s_last_apply_index;
	status->last_apply_kind = s_last_apply_kind;
	status->last_apply_age_ms = s_last_apply_valid ?
		monotonic_ms() - s_last_apply_ms : UINT32_MAX;
	status->last_error = s_last_error;
	xSemaphoreGive(s_lock);
	struct tm local = {0};
	status->clock_valid = clock_snapshot(&local, NULL);
	keemash_mesh_time_status_t time_status = {0};
	keemash_mesh_time_get_status(&time_status);
	status->time_sync_age_ms = time_status.last_sync_age_ms;
	if (status->clock_valid) {
		status->local_weekday = weekday_index(&local);
		status->local_minute = (uint16_t)(local.tm_hour * 60 + local.tm_min);
	}
	if (!status->clock_valid || !status->config.enabled) return;
	status->active_index = latest_point_index(&status->config,
		status->local_weekday, status->local_minute);
	status->next_index = next_point_index(&status->config,
		status->local_weekday, status->local_minute,
					    &status->next_in_minutes);
}
