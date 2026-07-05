#include "aht30_sensor.h"

#include "app_config.h"
#include "app_state.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "aht30";

static esp_err_t aht30_read(i2c_master_dev_handle_t dev, float *temp,
                            float *humidity)
{
  uint8_t init_cmd[] = {0xBE, 0x08, 0x00};
  ESP_RETURN_ON_ERROR(i2c_master_transmit(dev, init_cmd, sizeof(init_cmd),
                                          I2C_TIMEOUT_MS),
                      TAG, "AHT30 init failed");
  vTaskDelay(pdMS_TO_TICKS(10));

  uint8_t measure_cmd[] = {0xAC, 0x33, 0x00};
  ESP_RETURN_ON_ERROR(i2c_master_transmit(dev, measure_cmd,
                                          sizeof(measure_cmd),
                                          I2C_TIMEOUT_MS),
                      TAG, "AHT30 measure command failed");
  vTaskDelay(pdMS_TO_TICKS(80));

  uint8_t data[6] = {0};
  ESP_RETURN_ON_ERROR(i2c_master_receive(dev, data, sizeof(data),
                                         I2C_TIMEOUT_MS),
                      TAG, "AHT30 read failed");

  if (data[0] & 0x80)
  {
    return ESP_ERR_INVALID_STATE;
  }

  uint32_t raw_humidity =
      ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
  uint32_t raw_temp =
      (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) |
      data[5];

  *humidity = (float)raw_humidity * 100.0f / 1048576.0f;
  *temp = (float)raw_temp * 200.0f / 1048576.0f - 50.0f;
  return ESP_OK;
}

void aht30_task(void *arg)
{
  (void)arg;
  ESP_LOGI(TAG, "init AHT30 addr=0x%02X", AHT30_ADDR);

  i2c_master_dev_handle_t dev_handle = NULL;
  i2c_device_config_t dev_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = AHT30_ADDR,
      .scl_speed_hz = AHT30_I2C_FREQ_HZ,
  };
  ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_config,
                                            &dev_handle));

  while (1)
  {
    float temp = 0.0f;
    float humidity = 0.0f;
    esp_err_t ret = aht30_read(dev_handle, &temp, &humidity);
    if (ret == ESP_OK)
    {
      app_data_update_sensor(temp, humidity, true);
      ESP_LOGI(TAG, "AHT30 %.1f C %.1f %%", temp, humidity);
    }
    else
    {
      app_data_update_sensor(0, 0, false);
      ESP_LOGW(TAG, "AHT30 read error: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
