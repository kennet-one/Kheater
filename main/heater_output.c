#include "heater_output.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "heater_output";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static heater_output_state_t s_state;
static bool s_initialized;

static uint32_t pin_mask(void)
{
	return BIT64(CONFIG_KHEATER_FAN_GPIO) |
	       BIT64(CONFIG_KHEATER_ROTATION_GPIO) |
	       BIT64(CONFIG_KHEATER_HEAT_HIGH_GPIO) |
	       BIT64(CONFIG_KHEATER_HEAT_LOW_GPIO);
}

static void write_levels(const heater_output_state_t *state)
{
	gpio_set_level(CONFIG_KHEATER_FAN_GPIO, state->fan ? 1 : 0);
	gpio_set_level(CONFIG_KHEATER_ROTATION_GPIO, state->rotation ? 1 : 0);
	gpio_set_level(CONFIG_KHEATER_HEAT_HIGH_GPIO, state->heat_high ? 0 : 1);
	gpio_set_level(CONFIG_KHEATER_HEAT_LOW_GPIO, state->heat_low ? 0 : 1);
}

esp_err_t heater_output_init_safe(void)
{
	const heater_output_state_t safe = {0};

	/* Preload output latches before enabling the pins to avoid relay pulses. */
	write_levels(&safe);

	gpio_config_t config = {
		.pin_bit_mask = pin_mask(),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	esp_err_t err = gpio_config(&config);
	if (err != ESP_OK) return err;

	gpio_pullup_en(CONFIG_KHEATER_HEAT_HIGH_GPIO);
	gpio_pullup_en(CONFIG_KHEATER_HEAT_LOW_GPIO);
	gpio_pulldown_en(CONFIG_KHEATER_FAN_GPIO);
	gpio_pulldown_en(CONFIG_KHEATER_ROTATION_GPIO);
	write_levels(&safe);

	portENTER_CRITICAL(&s_lock);
	s_state = safe;
	s_initialized = true;
	portEXIT_CRITICAL(&s_lock);
	ESP_LOGI(TAG, "safe outputs: fan=0 low=0 high=0 rotation=0");
	return ESP_OK;
}

esp_err_t heater_output_apply(const heater_output_state_t *state)
{
	if (!state) return ESP_ERR_INVALID_ARG;
	portENTER_CRITICAL(&s_lock);
	if (!s_initialized) {
		portEXIT_CRITICAL(&s_lock);
		return ESP_ERR_INVALID_STATE;
	}
	write_levels(state);
	s_state = *state;
	portEXIT_CRITICAL(&s_lock);
	return ESP_OK;
}

void heater_output_get(heater_output_state_t *state)
{
	if (!state) return;
	portENTER_CRITICAL(&s_lock);
	*state = s_state;
	portEXIT_CRITICAL(&s_lock);
}
