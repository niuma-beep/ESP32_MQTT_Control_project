#include "app_state.h"

#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

app_data_t s_data = {
    .weather_status = "waiting",
    .sta_ip = "",
    .wifi_rssi = -127,
};

SemaphoreHandle_t s_data_lock;

void app_state_init(void)
{
  s_data_lock = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(s_data_lock ? ESP_OK : ESP_ERR_NO_MEM);

  uint64_t boot_us = esp_timer_get_time();
  xSemaphoreTake(s_data_lock, portMAX_DELAY);
  s_data.sensor_update_us = boot_us;
  s_data.weather_update_us = boot_us;
  s_data.wifi_update_us = boot_us;
  xSemaphoreGive(s_data_lock);
}

void app_data_snapshot(app_data_t *snapshot)
{
  xSemaphoreTake(s_data_lock, portMAX_DELAY);
  *snapshot = s_data;
  xSemaphoreGive(s_data_lock);
}

void app_data_update_sensor(float temp, float humidity, bool ok)
{
  xSemaphoreTake(s_data_lock, portMAX_DELAY);
  s_data.sensor_ok = ok;
  s_data.sensor_update_us = esp_timer_get_time();
  s_data.sensor_seq++;
  if (ok)
  {
    s_data.sensor_temp = temp;
    s_data.sensor_humidity = humidity;
  }
  xSemaphoreGive(s_data_lock);
}
