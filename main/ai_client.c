#include "ai_client.h"

#include <stdbool.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "ai_client";

#define AI_RESPONSE_MAX 768
#define AI_HTTP_TIMEOUT_MS 15000

typedef struct
{
  char data[AI_RESPONSE_MAX];
  size_t len;
} ai_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
  ai_response_t *response = (ai_response_t *)evt->user_data;
  if (!response)
  {
    return ESP_OK;
  }

  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0)
  {
    size_t available = sizeof(response->data) - response->len - 1;
    size_t copy_len = evt->data_len < available ? evt->data_len : available;
    if (copy_len > 0)
    {
      memcpy(response->data + response->len, evt->data, copy_len);
      response->len += copy_len;
      response->data[response->len] = '\0';
    }
  }
  return ESP_OK;
}

static ai_command_type_t parse_command_type(const char *action)
{
  if (!action)
  {
    return AI_COMMAND_NONE;
  }
  if (strcmp(action, "set_brightness") == 0)
  {
    return AI_COMMAND_SET_BRIGHTNESS;
  }
  if (strcmp(action, "set_display_mode") == 0 || strcmp(action, "set_mode") == 0)
  {
    return AI_COMMAND_SET_DISPLAY_MODE;
  }
  if (strcmp(action, "set_effect") == 0)
  {
    return AI_COMMAND_SET_EFFECT;
  }
  if (strcmp(action, "set_color") == 0)
  {
    return AI_COMMAND_SET_COLOR;
  }
  return AI_COMMAND_NONE;
}

static esp_err_t parse_ai_command(const char *json, ai_command_t *command)
{
  if (!json || !command)
  {
    return ESP_ERR_INVALID_ARG;
  }

  memset(command, 0, sizeof(*command));
  cJSON *root = cJSON_Parse(json);
  if (!root)
  {
    return ESP_ERR_INVALID_RESPONSE;
  }

  cJSON *action = cJSON_GetObjectItem(root, "action");
  command->type = parse_command_type(cJSON_GetStringValue(action));

  cJSON *value = cJSON_GetObjectItem(root, "value");
  if (cJSON_IsNumber(value))
  {
    command->value = value->valueint;
  }

  cJSON *mode = cJSON_GetObjectItem(root, "mode");
  cJSON *effect = cJSON_GetObjectItem(root, "effect");
  const char *text = cJSON_GetStringValue(mode);
  if (!text)
  {
    text = cJSON_GetStringValue(effect);
  }
  if (text)
  {
    strlcpy(command->text, text, sizeof(command->text));
  }

  cJSON *red = cJSON_GetObjectItem(root, "red");
  cJSON *green = cJSON_GetObjectItem(root, "green");
  cJSON *blue = cJSON_GetObjectItem(root, "blue");
  if (cJSON_IsNumber(red) && cJSON_IsNumber(green) && cJSON_IsNumber(blue))
  {
    command->red = (uint8_t)red->valueint;
    command->green = (uint8_t)green->valueint;
    command->blue = (uint8_t)blue->valueint;
  }

  cJSON_Delete(root);
  return command->type == AI_COMMAND_NONE ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t ai_client_send_audio(const int16_t *pcm, size_t sample_count,
                               uint32_t sample_rate_hz, ai_command_t *command)
{
  if (!pcm || sample_count == 0 || !command)
  {
    return ESP_ERR_INVALID_ARG;
  }

  ai_response_t response = {0};
  esp_http_client_config_t config = {
      .url = AI_VOICE_ENDPOINT,
      .method = HTTP_METHOD_POST,
      .timeout_ms = AI_HTTP_TIMEOUT_MS,
      .event_handler = http_event_handler,
      .user_data = &response,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client)
  {
    return ESP_FAIL;
  }

  char sample_rate[16];
  snprintf(sample_rate, sizeof(sample_rate), "%lu", (unsigned long)sample_rate_hz);
  esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
  esp_http_client_set_header(client, "X-Audio-Format", "pcm_s16le");
  esp_http_client_set_header(client, "X-Sample-Rate", sample_rate);
  esp_http_client_set_post_field(client, (const char *)pcm,
                                 sample_count * sizeof(int16_t));

  esp_err_t ret = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (ret != ESP_OK)
  {
    ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(ret));
    return ret;
  }
  if (status < 200 || status >= 300)
  {
    ESP_LOGW(TAG, "bad HTTP status: %d", status);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "AI response: %s", response.data);
  return parse_ai_command(response.data, command);
}