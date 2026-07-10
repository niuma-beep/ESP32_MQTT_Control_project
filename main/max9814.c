#include "max9814.h"

#include <limits.h>
#include <stdbool.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

static const char *TAG = "max9814";

#define MAX9814_ADC_UNIT ADC_UNIT_1
#define MAX9814_ADC_ATTEN ADC_ATTEN_DB_12
#define MAX9814_ADC_BITWIDTH ADC_BITWIDTH_12
#define MAX9814_SAMPLE_INTERVAL_US 500
#define MAX9814_TASK_SAMPLE_WINDOW_MS 50
#define MAX9814_TASK_PERIOD_MS 1000
#define MAX9814_ADC_FULL_SCALE 4095

#if CONFIG_IDF_TARGET_ESP32C3
#define MAX9814_ADC_CHANNEL ADC_CHANNEL_1
#else
#error "MAX9814 ADC channel is currently mapped for ESP32-C3 GPIO1 only."
#endif

static adc_oneshot_unit_handle_t s_adc_handle;
static bool s_initialized;

esp_err_t max9814_init(void)
{
  if (s_initialized)
  {
    return ESP_OK;
  }

  adc_oneshot_unit_init_cfg_t unit_config = {
      .unit_id = MAX9814_ADC_UNIT,
  };
  ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &s_adc_handle), TAG,
                      "ADC unit init failed");

  adc_oneshot_chan_cfg_t channel_config = {
      .atten = MAX9814_ADC_ATTEN,
      .bitwidth = MAX9814_ADC_BITWIDTH,
  };
  ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle,
                                                 MAX9814_ADC_CHANNEL,
                                                 &channel_config),
                      TAG, "ADC channel config failed");

  s_initialized = true;
  ESP_LOGI(TAG, "init MAX9814 voice GPIO%d ADC1_CH%d", VOICE_GPIO,
           MAX9814_ADC_CHANNEL);
  return ESP_OK;
}

esp_err_t max9814_read_raw(int *raw)
{
  if (!raw)
  {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_RETURN_ON_ERROR(max9814_init(), TAG, "MAX9814 init failed");
  return adc_oneshot_read(s_adc_handle, MAX9814_ADC_CHANNEL, raw);
}

esp_err_t max9814_sample(max9814_sample_t *sample, uint32_t window_ms)
{
  if (!sample || window_ms == 0)
  {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_RETURN_ON_ERROR(max9814_init(), TAG, "MAX9814 init failed");

  int raw_min = INT_MAX;
  int raw_max = 0;
  int64_t raw_sum = 0;
  uint32_t count = 0;
  int64_t start_us = esp_timer_get_time();
  int64_t window_us = (int64_t)window_ms * 1000;

  while ((esp_timer_get_time() - start_us) < window_us)
  {
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, MAX9814_ADC_CHANNEL, &raw);
    if (ret != ESP_OK)
    {
      return ret;
    }

    if (raw < raw_min)
    {
      raw_min = raw;
    }
    if (raw > raw_max)
    {
      raw_max = raw;
    }
    raw_sum += raw;
    count++;
    esp_rom_delay_us(MAX9814_SAMPLE_INTERVAL_US);
  }

  if (count == 0)
  {
    return ESP_ERR_INVALID_STATE;
  }

  int peak_to_peak = raw_max - raw_min;
  sample->raw_min = raw_min;
  sample->raw_max = raw_max;
  sample->raw_avg = (int)(raw_sum / count);
  sample->peak_to_peak = peak_to_peak;
  sample->level = (uint8_t)((peak_to_peak * 100) / MAX9814_ADC_FULL_SCALE);
  return ESP_OK;
}

void max9814_task(void *arg)
{
  (void)arg;

  if (max9814_init() != ESP_OK)
  {
    ESP_LOGE(TAG, "MAX9814 task stopped");
    vTaskDelete(NULL);
  }

  while (1)
  {
    max9814_sample_t sample = {0};
    esp_err_t ret = max9814_sample(&sample, MAX9814_TASK_SAMPLE_WINDOW_MS);
    if (ret == ESP_OK)
    {
      ESP_LOGI(TAG, "min=%d max=%d avg=%d p2p=%d level=%u",
               sample.raw_min, sample.raw_max, sample.raw_avg,
               sample.peak_to_peak, sample.level);
    }
    else
    {
      ESP_LOGW(TAG, "sample failed: %s", esp_err_to_name(ret));
    }

    vTaskDelay(pdMS_TO_TICKS(MAX9814_TASK_PERIOD_MS));
  }
}
