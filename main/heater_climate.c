#include "heater_climate.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "heater_i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "keemash_mesh_node.h"
#include "sdkconfig.h"

static StaticSemaphore_t s_storage;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static climate_policy_t s_policy;
static esp_err_t s_error = ESP_ERR_NOT_FOUND;

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

void heater_climate_get(heater_climate_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->sensor_error = ESP_ERR_INVALID_STATE;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    out->now_ms = now_ms();
    out->source = climate_select(&s_policy, out->now_ms);
    out->policy = s_policy;
    out->sensor_error = s_error;
    xSemaphoreGive(s_lock);
}

static unsigned age(const climate_sample_t *s, uint64_t now)
{
    if (!s->valid || now < s->sampled_ms) return 65535;
    uint64_t seconds = (now - s->sampled_ms) / 1000;
    return seconds > 65534 ? 65534 : (unsigned)seconds;
}

void heater_climate_format(char *out, size_t size)
{
    heater_climate_status_t s;
    heater_climate_get(&s);
    bool local = s.policy.local.valid && s.now_ms < s.policy.local.expires_ms;
    bool ext = s.policy.external.valid && s.now_ms < s.policy.external.expires_ms;
    unsigned reason = !s.policy.enabled ? 0 : !ext ? 1 : s.policy.confirmations < 2 ? 2 : 0;
    if (s.source == CLIMATE_NONE) reason = 3;
    snprintf(out, size,
        "HC1 s=%u r=%u z=%u v=%08lx i=%ld h=%ld ia=%u e=%ld ea=%u m=%012llx t=%ld",
        (unsigned)s.source, reason, s.policy.enabled,
        (unsigned long)s.policy.revision,
        (long)(local ? s.policy.local.temperature : 32767),
        (long)(local ? s.policy.local.humidity : 32767),
        local ? age(&s.policy.local, s.now_ms) : 65535,
        (long)(ext ? s.policy.external.temperature : 32767),
        ext ? age(&s.policy.external, s.now_ms) : 65535,
        (unsigned long long)s.policy.source_mac,
        (long)(s.source == CLIMATE_ZONE ? s.policy.external.temperature :
               s.source == CLIMATE_INTERNAL ? s.policy.local.temperature : 32767));
}

bool heater_climate_command(const char *text, esp_err_t *error)
{
    if (!text || !error) return false;
    if (strncmp(text, "HC:", 3) && strncmp(text, "HT:", 3) &&
        strncmp(text, "HX:", 3)) return false;
    *error = ESP_ERR_INVALID_ARG;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        *error = ESP_ERR_TIMEOUT;
        return true;
    }
    unsigned rev = 0, enable = 0, session = 0, generation = 0;
    unsigned long long mac = 0, epoch = 0;
    int t = 0, n = 0;
    bool ok = false;
    if (sscanf(text, "HC:%8x:%1u:%12llx%n", &rev, &enable, &mac, &n) == 3 &&
        text[n] == 0 && enable <= 1) {
        ok = climate_configure(&s_policy, rev, enable != 0, mac);
    } else if (sscanf(text, "HT:%8x:%8x:%8x:%13llu:%6d%n",
                      &rev, &session, &generation, &epoch, &t, &n) == 5 && text[n] == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t wall = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        ok = climate_external(&s_policy, rev, session, generation, epoch, t,
            wall, now_ms(), CONFIG_KHEATER_EXTERNAL_TEMP_FRESH_MS);
    } else if (sscanf(text, "HX:%8x%n", &rev, &n) == 1 && text[n] == 0 &&
               s_policy.configured && rev == s_policy.revision) {
        s_policy.external.valid = false;
        s_policy.confirmations = 0;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    *error = ok ? ESP_OK : ESP_ERR_INVALID_ARG;
    return true;
}

static void climate_task(void *arg)
{
    (void)arg;
    i2c_master_dev_handle_t device = NULL;
    TickType_t wake = xTaskGetTickCount();
    uint64_t publish_ms = 0, warning_ms = 0, sample_ms = 0;
    uint32_t generation = 0;
    bool last_valid = false, published = false;
    bool valid = false;
    int32_t t = 0, h = 0;
    for (;;) {
        uint64_t now = now_ms();
        if (!generation || now - sample_ms >= 2000) {
        esp_err_t err = device ? ESP_OK : heater_i2c_add(0x38, &device);
        uint8_t bytes[7] = {0};
        if (err == ESP_OK) {
            const uint8_t trigger[] = {0xac, 0x33, 0x00};
            err = i2c_master_transmit(device, trigger, sizeof(trigger), 100);
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(80));
                for (unsigned attempt = 0; attempt < 4; ++attempt) {
                    err = i2c_master_receive(device, bytes, sizeof(bytes), 100);
                    if (err != ESP_OK || !(bytes[0] & 0x80)) break;
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                if (err == ESP_OK && !climate_aht30_decode(bytes, &t, &h)) err = ESP_ERR_INVALID_CRC;
            }
        }
        now = now_ms();
        sample_ms = now;
        valid = err == ESP_OK;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_error = err;
            if (valid) {
                s_policy.local = (climate_sample_t){ .valid = true, .temperature = t,
                    .humidity = h, .sampled_ms = now,
                    .expires_ms = now + CONFIG_KHEATER_INTERNAL_TEMP_FRESH_MS };
            } else s_policy.local.valid = false;
            xSemaphoreGive(s_lock);
        }
        ++generation;
        if (!valid) {
            if (!warning_ms || now - warning_ms >= 30000) {
                ESP_LOGW("heater_climate", "AHT30 unavailable: %s", esp_err_to_name(err));
                warning_ms = now;
            }
            if (device && heater_i2c_remove(device) == ESP_OK) device = NULL;
        }
        }
        if (!published || valid != last_valid || now - publish_ms >= 5000) {
            mesh_v2_sensor_snapshot_payload_t sensor = {
                .generation = generation, .sample_uptime_ms = (uint32_t)sample_ms,
                .flags = MESH_V2_SENSOR_FLAG_PERIODIC, .count = 2,
            };
            uint8_t status = valid ? MESH_V2_SENSOR_STATUS_VALID | MESH_V2_SENSOR_STATUS_CALIBRATED
                                   : MESH_V2_SENSOR_STATUS_ERROR;
            sensor.entries[0] = (mesh_v2_sensor_entry_t){
                .metric_id = MESH_V2_SENSOR_METRIC_TEMPERATURE_C, .status = status,
                .scale10 = -2, .value = t };
            sensor.entries[1] = (mesh_v2_sensor_entry_t){
                .metric_id = MESH_V2_SENSOR_METRIC_HUMIDITY_RH, .status = status,
                .scale10 = -2, .value = h };
            if (mesh_v2_node_send_sensor_snapshot(&sensor) == ESP_OK) {
                publish_ms = now;
                last_valid = valid;
                published = true;
            }
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));
    }
}

esp_err_t heater_climate_start(void)
{
    if (s_task) return ESP_OK;
    if (!climate_selftest()) return ESP_FAIL;
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_storage);
    if (!s_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(climate_task, "heater_climate", 4096, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
