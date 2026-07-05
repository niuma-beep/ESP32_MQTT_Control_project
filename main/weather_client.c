#include "weather_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_state.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys/param.h"

static const char *TAG = "weather";

typedef struct
{
  char *buffer;
  int length;
  int capacity;
} http_capture_t;

static esp_err_t weather_http_event_handler(esp_http_client_event_t *evt)
{
  http_capture_t *capture = evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 &&
      capture && capture->buffer)
  {
    int copy_len = MIN(evt->data_len, capture->capacity - capture->length - 1);
    if (copy_len > 0)
    {
      memcpy(capture->buffer + capture->length, evt->data, copy_len);
      capture->length += copy_len;
      capture->buffer[capture->length] = '\0';
    }
  }
  return ESP_OK;
}

static bool split_weather_field(char **cursor, char *dst, size_t dst_size)
{
  if (!cursor || !*cursor || !dst || dst_size == 0)
  {
    return false;
  }

  char *start = *cursor;
  char *end = strchr(start, '|');
  if (end)
  {
    *end = '\0';
    *cursor = end + 1;
  }
  else
  {
    *cursor = NULL;
  }

  while (*start == ' ' || *start == '\r' || *start == '\n' || *start == '\t')
  {
    start++;
  }
  char *tail = start + strlen(start);
  while (tail > start &&
         (tail[-1] == ' ' || tail[-1] == '\r' || tail[-1] == '\n' ||
          tail[-1] == '\t'))
  {
    *--tail = '\0';
  }

  strlcpy(dst, start, dst_size);
  return dst[0] != '\0';
}

static void normalize_weather_number(char *value)
{
  if (!value)
  {
    return;
  }

  char out[16] = {0};
  size_t pos = 0;
  for (size_t i = 0; value[i] && pos < sizeof(out) - 1; i++)
  {
    char c = value[i];
    if ((c >= '0' && c <= '9') || c == '-' || c == '.')
    {
      out[pos++] = c;
    }
  }
  strlcpy(value, out, 16);
}

static void parse_weather_text(char *payload)
{
  bool ok = false;
  char city[sizeof(s_data.city)] = {0};
  char desc[sizeof(s_data.weather_desc)] = {0};
  char temp[sizeof(s_data.weather_temp)] = {0};
  char hum[sizeof(s_data.weather_humidity)] = {0};
  char wind[sizeof(s_data.wind_scale)] = {0};
  char *cursor = payload;

  ok = split_weather_field(&cursor, city, sizeof(city)) &&
       split_weather_field(&cursor, desc, sizeof(desc)) &&
       split_weather_field(&cursor, temp, sizeof(temp)) &&
       split_weather_field(&cursor, hum, sizeof(hum));
  if (cursor)
  {
    strlcpy(wind, cursor, sizeof(wind));
  }
  normalize_weather_number(temp);
  normalize_weather_number(hum);
  ok = ok && temp[0] != '\0' && hum[0] != '\0';

  xSemaphoreTake(s_data_lock, portMAX_DELAY);
  if (ok)
  {
    strlcpy(s_data.city, city, sizeof(s_data.city));
    strlcpy(s_data.weather_desc, desc, sizeof(s_data.weather_desc));
    strlcpy(s_data.weather_temp, temp, sizeof(s_data.weather_temp));
    strlcpy(s_data.weather_humidity, hum, sizeof(s_data.weather_humidity));
    strlcpy(s_data.wind_dir, "", sizeof(s_data.wind_dir));
    strlcpy(s_data.wind_scale, wind, sizeof(s_data.wind_scale));
    strlcpy(s_data.weather_time, "", sizeof(s_data.weather_time));
    strlcpy(s_data.weather_status, "weather updated",
            sizeof(s_data.weather_status));
    s_data.weather_ok = true;
  }
  else
  {
    strlcpy(s_data.weather_status, "weather parse failed",
            sizeof(s_data.weather_status));
    s_data.weather_ok = false;
    s_data.weather_desc[0] = '\0';
  }
  s_data.weather_update_us = esp_timer_get_time();
  s_data.weather_seq++;
  xSemaphoreGive(s_data_lock);
}

static void fetch_weather_once(void)
{
  char response[256] = {0};
  char weather_url[128];
  snprintf(weather_url, sizeof(weather_url),
           "http://%s/%s?format=%%l%%7C%%C%%7C%%t%%7C%%h%%7C%%w",
           WEATHER_HOST,
           WEATHER_LOCATION);
  http_capture_t capture = {
      .buffer = response,
      .capacity = sizeof(response),
  };

  esp_http_client_config_t config = {
      .url = weather_url,
      .event_handler = weather_http_event_handler,
      .user_data = &capture,
      .timeout_ms = 8000,
      .keep_alive_enable = false,
      .disable_auto_redirect = false,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client)
  {
    return;
  }

  esp_http_client_set_header(client, "User-Agent", "curl/8.0");
  esp_http_client_set_header(client, "Accept", "text/plain,*/*");
  esp_http_client_set_header(client, "Connection", "close");

  esp_err_t ret = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  int content_length = esp_http_client_get_content_length(client);
  esp_http_client_cleanup(client);

  if (ret == ESP_OK && status == 200 && capture.length > 0)
  {
    parse_weather_text(response);
  }
  else
  {
    ESP_LOGW(TAG, "weather HTTP failed status=%d ret=%s received=%d/%d: %s",
             status, esp_err_to_name(ret), capture.length, content_length,
             response);
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    snprintf(s_data.weather_status, sizeof(s_data.weather_status),
             "weather HTTP failed %d %s %d/%d", status, esp_err_to_name(ret),
             capture.length, content_length);
    s_data.weather_ok = false;
    s_data.weather_desc[0] = '\0';
    s_data.weather_update_us = esp_timer_get_time();
    s_data.weather_seq++;
    xSemaphoreGive(s_data_lock);
  }
}

void weather_task(void *arg)
{
  (void)arg;
  while (1)
  {
    bool connected = false;
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    connected = s_data.sta_connected;
    xSemaphoreGive(s_data_lock);

    if (connected)
    {
      fetch_weather_once();
      vTaskDelay(pdMS_TO_TICKS(10 * 60 * 1000));
    }
    else
    {
      xSemaphoreTake(s_data_lock, portMAX_DELAY);
      strlcpy(s_data.weather_status, "router wifi not connected",
              sizeof(s_data.weather_status));
      s_data.weather_ok = false;
      xSemaphoreGive(s_data_lock);
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}
