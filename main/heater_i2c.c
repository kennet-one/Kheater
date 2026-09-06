#include "heater_i2c.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static i2c_master_bus_handle_t s_bus;
static StaticSemaphore_t s_storage;
static SemaphoreHandle_t s_lock;

esp_err_t heater_i2c_remove(i2c_master_dev_handle_t device)
{
    if (!s_lock || !device) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = i2c_master_bus_rm_device(device);
    xSemaphoreGive(s_lock);
    return err;
}

/* Called once before starting display/sensor tasks. IDF serializes transfers. */
esp_err_t heater_i2c_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_storage);
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t heater_i2c_add(uint8_t address, i2c_master_dev_handle_t *device)
{
    if (!device || !s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (!s_bus) {
        i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = CONFIG_KHEATER_I2C_SDA_GPIO,
            .scl_io_num = CONFIG_KHEATER_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        err = i2c_new_master_bus(&config, &s_bus);
    }
    if (err == ESP_OK) {
        i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(s_bus, &config, device);
    }
    xSemaphoreGive(s_lock);
    return err;
}
