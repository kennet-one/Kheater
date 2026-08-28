#include "heater_display.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heater_controller.h"
#include "heater_schedule.h"
#include "sdkconfig.h"
#include "u8g2.h"

#ifndef PROGMEM
#define PROGMEM
#endif
#include "../IMG.h"

#define DISPLAY_I2C_PORT I2C_NUM_0
#define DISPLAY_TX_BUFFER 192
#define DISPLAY_REFRESH_MS 500U

static const char *TAG = "heater_display";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_parent_connected;
static bool s_reliable_ready;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static u8g2_t s_u8g2;
static uint8_t s_tx_buffer[DISPLAY_TX_BUFFER];
static size_t s_tx_length;
static TaskHandle_t s_task;
static uint32_t s_tx_error_count;

static bool flush_i2c(void)
{
	if (!s_device || s_tx_length == 0) {
		s_tx_length = 0;
		return true;
	}
	esp_err_t err = i2c_master_transmit(s_device, s_tx_buffer, s_tx_length, 100);
	s_tx_length = 0;
	if (err != ESP_OK) {
		s_tx_error_count++;
		if (s_tx_error_count == 1 || (s_tx_error_count % 32U) == 0) {
			ESP_LOGW(TAG, "SH1106 transfer failed (%lu): %s",
				 (unsigned long)s_tx_error_count, esp_err_to_name(err));
		}
	}
	return err == ESP_OK;
}

static uint8_t u8x8_byte_i2c(u8x8_t *u8x8, uint8_t message,
			      uint8_t arg_int, void *arg_ptr)
{
	(void)u8x8;
	switch (message) {
	case U8X8_MSG_BYTE_INIT:
		return 1;
	case U8X8_MSG_BYTE_SET_DC:
		/* The SSD13xx I2C CAD layer emits its own 0x00/0x40 control byte. */
		return 1;
	case U8X8_MSG_BYTE_START_TRANSFER:
		s_tx_length = 0;
		return 1;
	case U8X8_MSG_BYTE_SEND: {
		const uint8_t *bytes = arg_ptr;
		while (arg_int > 0) {
			size_t available = sizeof(s_tx_buffer) - s_tx_length;
			if (available == 0) {
				if (!flush_i2c()) return 0;
				available = sizeof(s_tx_buffer);
			}
			size_t chunk = arg_int < available ? arg_int : available;
			memcpy(s_tx_buffer + s_tx_length, bytes, chunk);
			s_tx_length += chunk;
			bytes += chunk;
			arg_int -= (uint8_t)chunk;
		}
		return 1;
	}
	case U8X8_MSG_BYTE_END_TRANSFER:
		return flush_i2c() ? 1 : 0;
	default:
		return 0;
	}
}

static uint8_t u8x8_gpio_delay(u8x8_t *u8x8, uint8_t message,
				uint8_t arg_int, void *arg_ptr)
{
	(void)u8x8;
	(void)arg_ptr;
	switch (message) {
	case U8X8_MSG_DELAY_MILLI:
		vTaskDelay(pdMS_TO_TICKS(arg_int));
		break;
	case U8X8_MSG_DELAY_10MICRO:
		esp_rom_delay_us((uint32_t)arg_int * 10U);
		break;
	case U8X8_MSG_DELAY_100NANO:
		esp_rom_delay_us(1);
		break;
	default:
		break;
	}
	return 1;
}

static void draw_logo(void)
{
	u8g2_ClearBuffer(&s_u8g2);
	/* The original asset uses U8g2 drawBitmap bit order, not XBM bit order. */
	u8g2_DrawBitmap(&s_u8g2, 32, 0, 8, 64, logo);
	u8g2_SendBuffer(&s_u8g2);
}

static void draw_right_aligned(const char *text, uint8_t y)
{
	uint16_t width = u8g2_GetStrWidth(&s_u8g2, text);
	u8g2_DrawStr(&s_u8g2, width < 128U ? 128U - width : 0U, y, text);
}

static uint8_t draw_output_badge(uint8_t x, const char *label, bool active)
{
	uint8_t width = (uint8_t)u8g2_GetStrWidth(&s_u8g2, label) + 6U;
	if (active) {
		u8g2_DrawBox(&s_u8g2, x, 43, width, 10);
		u8g2_SetDrawColor(&s_u8g2, 0);
		u8g2_DrawStr(&s_u8g2, x + 3U, 51, label);
		u8g2_SetDrawColor(&s_u8g2, 1);
	} else {
		u8g2_DrawFrame(&s_u8g2, x, 43, width, 10);
		u8g2_DrawStr(&s_u8g2, x + 3U, 51, label);
	}
	return (uint8_t)(x + width + 3U);
}

static const char *schedule_action_name(heater_schedule_action_t action)
{
	switch (action) {
	case HEATER_SCHEDULE_ACTION_OFF: return "OFF";
	case HEATER_SCHEDULE_ACTION_AUTO: return "AUTO";
	case HEATER_SCHEDULE_ACTION_FAN: return "FAN";
	case HEATER_SCHEDULE_ACTION_LOW: return "LOW";
	case HEATER_SCHEDULE_ACTION_HIGH: return "HIGH";
	case HEATER_SCHEDULE_ACTION_MAX: return "MAX";
	case HEATER_SCHEDULE_ACTION_UNCHANGED: return "SET";
	default: return "?";
	}
}

static void format_bottom_line(char *line, size_t line_size,
			       const heater_controller_status_t *status,
			       const heater_schedule_status_t *schedule)
{
	if (status->cooldown_active) {
		snprintf(line, line_size, "COOLDOWN %llus",
			 (unsigned long long)(status->cooldown_remaining_ms / 1000ULL));
		return;
	}
	if (status->stop_reason == HEATER_STOP_TEMP_STALE) {
		snprintf(line, line_size, "TEMP STALE");
		return;
	}
	if (status->stop_reason == HEATER_STOP_TEMP_INVALID) {
		snprintf(line, line_size, "TEMP INVALID");
		return;
	}
	if (status->stop_reason == HEATER_STOP_MANUAL_TIMEOUT) {
		snprintf(line, line_size, "HEAT TIMEOUT");
		return;
	}
	if (status->manual_remaining_ms > 0 &&
	    (status->mode == HEATER_MODE_LOW || status->mode == HEATER_MODE_HIGH ||
	     status->mode == HEATER_MODE_MAX)) {
		snprintf(line, line_size, "LIMIT %llum",
			 (unsigned long long)(status->manual_remaining_ms / 60000ULL));
		return;
	}
	if (status->auto_enabled && !status->temperature_valid) {
		snprintf(line, line_size, "WAIT TEMP");
		return;
	}
	if (schedule->config.enabled && schedule->clock_valid &&
	    schedule->next_index < schedule->config.count) {
		const heater_schedule_point_t *next =
			&schedule->config.points[schedule->next_index];
		snprintf(line, line_size, "NEXT %02u:%02u %s",
			 next->minute_of_day / 60U, next->minute_of_day % 60U,
			 schedule_action_name(next->action));
		return;
	}
	if (schedule->config.enabled && !schedule->clock_valid) {
		snprintf(line, line_size, "SCHEDULE WAIT TIME");
		return;
	}
	if (status->auto_enabled && status->temperature_valid) {
		snprintf(line, line_size, "TEMP AGE %llus",
			 (unsigned long long)(status->temperature_age_ms / 1000ULL));
		return;
	}
	snprintf(line, line_size, "STANDBY");
}

static void draw_status(void)
{
	heater_controller_status_t status;
	heater_schedule_status_t schedule;
	heater_controller_get_status(&status);
	heater_schedule_get_status(&schedule);
	bool parent;
	bool reliable;
	portENTER_CRITICAL(&s_state_lock);
	parent = s_parent_connected;
	reliable = s_reliable_ready;
	portEXIT_CRITICAL(&s_state_lock);

	char line[32];
	char right[16];
	u8g2_ClearBuffer(&s_u8g2);
	u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tf);
	snprintf(line, sizeof(line), "KHEATER %s",
		 reliable ? "V2" : (parent ? "MESH" : "OFF"));
	u8g2_DrawStr(&s_u8g2, 0, 9, line);

	u8g2_SetFont(&s_u8g2, u8g2_font_helvB14_tf);
	u8g2_DrawStr(&s_u8g2, 0, 27,
		     heater_controller_mode_name(status.mode));
	if (status.temperature_valid) {
		snprintf(right, sizeof(right), "%.1fC", status.temperature_c);
		draw_right_aligned(right, 27);
	}

	u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tf);
	snprintf(line, sizeof(line), "SET %.1fC", status.setpoint_c);
	u8g2_DrawStr(&s_u8g2, 0, 39, line);
	if (status.temperature_valid) {
		uint64_t age_s = status.temperature_age_ms / 1000ULL;
		if (age_s > 999ULL) age_s = 999ULL;
		snprintf(right, sizeof(right), "AGE %llus", (unsigned long long)age_s);
		draw_right_aligned(right, 39);
	}

	u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
	uint8_t x = draw_output_badge(0, "FAN", status.outputs.fan);
	x = draw_output_badge(x, "LOW", status.outputs.heat_low);
	x = draw_output_badge(x, "HIGH", status.outputs.heat_high);
	draw_output_badge(x, "ROT", status.outputs.rotation);

	u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tf);
	format_bottom_line(line, sizeof(line), &status, &schedule);
	u8g2_DrawStr(&s_u8g2, 0, 62, line);
	u8g2_SendBuffer(&s_u8g2);
}

static void display_task(void *arg)
{
	(void)arg;
	draw_logo();
	uint64_t logo_until = (uint64_t)esp_timer_get_time() / 1000ULL +
			      CONFIG_KHEATER_LOGO_TIME_MS;
	while ((uint64_t)esp_timer_get_time() / 1000ULL < logo_until) {
		vTaskDelay(pdMS_TO_TICKS(50));
	}
	for (;;) {
		draw_status();
		vTaskDelay(pdMS_TO_TICKS(DISPLAY_REFRESH_MS));
	}
}

esp_err_t heater_display_start(void)
{
#if !CONFIG_KHEATER_DISPLAY_ENABLE
	return ESP_ERR_NOT_SUPPORTED;
#else
	if (s_task) return ESP_OK;
	i2c_master_bus_config_t bus_config = {
		.i2c_port = DISPLAY_I2C_PORT,
		.sda_io_num = CONFIG_KHEATER_I2C_SDA_GPIO,
		.scl_io_num = CONFIG_KHEATER_I2C_SCL_GPIO,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
	if (err != ESP_OK) return err;
	i2c_device_config_t device_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = CONFIG_KHEATER_I2C_ADDRESS,
		.scl_speed_hz = 400000,
	};
	err = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
	if (err != ESP_OK) return err;
	uint8_t probe = 0x00;
	err = i2c_master_transmit(s_device, &probe, 1, 100);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "SH1106 not responding at 0x%02x: %s",
			 CONFIG_KHEATER_I2C_ADDRESS, esp_err_to_name(err));
		return err;
	}

	u8g2_Setup_sh1106_i2c_128x64_noname_f(
		&s_u8g2, U8G2_R0, u8x8_byte_i2c, u8x8_gpio_delay);
	u8g2_InitDisplay(&s_u8g2);
	u8g2_SetPowerSave(&s_u8g2, 0);
	u8g2_SetBitmapMode(&s_u8g2, 0);
	u8g2_ClearDisplay(&s_u8g2);
	if (xTaskCreate(display_task, "heater_display", 4096, NULL, 3, &s_task) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	ESP_LOGI(TAG, "SH1106 ready on SDA=%d SCL=%d addr=0x%02x",
		 CONFIG_KHEATER_I2C_SDA_GPIO, CONFIG_KHEATER_I2C_SCL_GPIO,
		 CONFIG_KHEATER_I2C_ADDRESS);
	return ESP_OK;
#endif
}

void heater_display_set_mesh_state(bool parent_connected, bool reliable_ready)
{
	portENTER_CRITICAL(&s_state_lock);
	s_parent_connected = parent_connected;
	s_reliable_ready = reliable_ready;
	portEXIT_CRITICAL(&s_state_lock);
}
