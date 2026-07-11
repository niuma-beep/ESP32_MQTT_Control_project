#include "voice_control.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ai_client.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "max9814.h"
#include "rgb_led.h"

static const char *TAG = "voice";

#define ADC_MIDPOINT 2048
#define PCM_GAIN_SHIFT 4
#define VOICE_UPLOAD_WAIT_MS 10

typedef struct
{
  int16_t *pcm;
  size_t sample_count;
  size_t max_samples;
  bool busy;
  volatile bool recording;
  volatile bool task_running;
  TaskHandle_t task;
} voice_state_t;

static voice_state_t s_voice;

static int16_t raw_to_pcm(int raw)
{
  int32_t sample = (raw - ADC_MIDPOINT) << PCM_GAIN_SHIFT;
  if (sample > INT16_MAX)
  {
    sample = INT16_MAX;
  }
  if (sample < INT16_MIN)
  {
    sample = INT16_MIN;
  }
  return (int16_t)sample;
}

static void voice_record_task(void *arg)
{
  (void)arg;
  const uint32_t interval_us = 1000000UL / VOICE_SAMPLE_RATE_HZ;

  s_voice.task_running = true;
  while (s_voice.recording && s_voice.sample_count < s_voice.max_samples)
  {
    int raw = 0;
    if (max9814_read_raw(&raw) == ESP_OK)
    {
      s_voice.pcm[s_voice.sample_count++] = raw_to_pcm(raw);
    }
    esp_rom_delay_us(interval_us);
  }

  s_voice.recording = false;
  s_voice.task_running = false;
  s_voice.task = NULL;
  vTaskDelete(NULL);
}

static void voice_release_buffer(void)
{
  free(s_voice.pcm);
  memset(&s_voice, 0, sizeof(s_voice));
}

esp_err_t voice_control_start(void)
{
  if (s_voice.busy)
  {
    return ESP_ERR_INVALID_STATE;
  }

  size_t max_samples = ((size_t)VOICE_SAMPLE_RATE_HZ * VOICE_MAX_RECORD_MS) / 1000;
  int16_t *pcm = (int16_t *)calloc(max_samples, sizeof(int16_t));
  if (!pcm)
  {
    ESP_LOGE(TAG, "no memory for voice buffer: %u samples", (unsigned)max_samples);
    return ESP_ERR_NO_MEM;
  }

  s_voice.pcm = pcm;
  s_voice.max_samples = max_samples;
  s_voice.sample_count = 0;
  s_voice.recording = true;
  s_voice.busy = true;

  BaseType_t ok = xTaskCreate(voice_record_task, "voice_record", 4096, NULL, 6,
                              &s_voice.task);
  if (ok != pdPASS)
  {
    voice_release_buffer();
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "recording start, max %u ms", VOICE_MAX_RECORD_MS);
  return ESP_OK;
}

void voice_control_cancel(void)
{
  if (!s_voice.busy)
  {
    return;
  }

  s_voice.recording = false;
  while (s_voice.task_running)
  {
    vTaskDelay(pdMS_TO_TICKS(VOICE_UPLOAD_WAIT_MS));
  }
  ESP_LOGI(TAG, "recording canceled");
  voice_release_buffer();
}

static void execute_ai_command(const ai_command_t *command)
{
  switch (command->type)
  {
  case AI_COMMAND_SET_BRIGHTNESS:
  {
    int value = command->value;
    if (value < 0)
    {
      value = 0;
    }
    if (value > CLOCK_LED_MAX_BRIGHTNESS)
    {
      value = CLOCK_LED_MAX_BRIGHTNESS;
    }
    clock_led_set_brightness((uint8_t)value);
    ESP_LOGI(TAG, "AI brightness -> %d", value);
    break;
  }
  case AI_COMMAND_SET_DISPLAY_MODE:
    if (command->text[0] != '\0')
    {
      clock_led_set_mode(command->text);
      ESP_LOGI(TAG, "AI display mode -> %s", command->text);
    }
    break;
  case AI_COMMAND_SET_EFFECT:
    if (command->text[0] != '\0')
    {
      clock_led_set_effect(command->text);
      ESP_LOGI(TAG, "AI effect -> %s", command->text);
    }
    break;
  case AI_COMMAND_SET_COLOR:
    clock_led_set_color(command->red, command->green, command->blue);
    ESP_LOGI(TAG, "AI color -> %u,%u,%u", command->red, command->green,
             command->blue);
    break;
  default:
    ESP_LOGW(TAG, "AI command ignored");
    break;
  }
}

esp_err_t voice_control_stop_and_execute(void)
{
  if (!s_voice.busy)
  {
    return ESP_ERR_INVALID_STATE;
  }

  s_voice.recording = false;
  while (s_voice.task_running)
  {
    vTaskDelay(pdMS_TO_TICKS(VOICE_UPLOAD_WAIT_MS));
  }

  size_t min_samples = ((size_t)VOICE_SAMPLE_RATE_HZ * VOICE_MIN_RECORD_MS) / 1000;
  if (s_voice.sample_count < min_samples)
  {
    ESP_LOGW(TAG, "recording too short: %u samples", (unsigned)s_voice.sample_count);
    voice_release_buffer();
    return ESP_ERR_INVALID_SIZE;
  }

  ESP_LOGI(TAG, "uploading %u samples", (unsigned)s_voice.sample_count);
  ai_command_t command = {0};
  esp_err_t ret = ai_client_send_audio(s_voice.pcm, s_voice.sample_count,
                                       VOICE_SAMPLE_RATE_HZ, &command);
  if (ret == ESP_OK)
  {
    execute_ai_command(&command);
  }
  else
  {
    ESP_LOGW(TAG, "AI request failed: %s", esp_err_to_name(ret));
  }

  voice_release_buffer();
  return ret;
}

bool voice_control_is_recording(void)
{
  return s_voice.busy && s_voice.recording;
}