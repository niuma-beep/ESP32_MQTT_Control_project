#include "aht30_sensor.h"
#include "app_state.h"
#include "buttons.h"
#include "dns_server.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "max9814.h"
#include "nvs_flash.h"
#include "oled_ui.h"
#include "rgb_led.h"
#include "weather_client.h"
#include "web_server.h"
#include "wifi_portal.h"
#include "mqtt_client_app.h"

static const char *TAG = "app_main";

static esp_err_t init_nvs(void)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }

  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG,
             "NVS init failed: %s. WiFi portal will not start. "
             "Check that the flashed partition table has a partition named 'nvs'.",
             esp_err_to_name(ret));
  }
  return ret;
}

static void reduce_http_log_noise(void)
{
  esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
  esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
}

void app_main(void)
{
  app_state_init();
  reduce_http_log_noise();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  if (init_nvs() != ESP_OK)
  {
    return;
  }

  esp_netif_t *ap_netif = NULL;
  wifi_init_apsta(&ap_netif);
  set_captive_portal_options(ap_netif);
  start_webserver();
  mqtt_client_app_start();

  xTaskCreate(dns_server_task, "dns_server", 4096, ap_netif, 5, NULL);
  xTaskCreate(weather_task, "weather_task", 6144, NULL, 5, NULL);
  xTaskCreate(led_task, "led_task", 4096, NULL, 5, NULL);
  xTaskCreate(buttons_task, "buttons_task", 3072, NULL, 5, NULL);
  xTaskCreate(max9814_task, "max9814_task", 3072, NULL, 5, NULL);

  if (i2c_bus_init() == ESP_OK)
  {
    xTaskCreate(aht30_task, "aht30_task", 4096, NULL, 5, NULL);
    xTaskCreate(oled_task, "oled_task", 4096, NULL, 5, NULL);
  }
}