#include "i2c_bus.h"

#include "app_config.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";

i2c_master_bus_handle_t s_i2c_bus;

esp_err_t i2c_bus_init(void)
{
  i2c_master_bus_config_t bus_config = {
      .i2c_port = AHT30_I2C_PORT,
      .sda_io_num = I2C_SDA_GPIO,
      .scl_io_num = I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t ret = i2c_new_master_bus(&bus_config, &s_i2c_bus);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
    return ret;
  }
  ESP_LOGI(TAG, "I2C bus initialized SDA=%d SCL=%d", I2C_SDA_GPIO,
           I2C_SCL_GPIO);
  return ESP_OK;
}
