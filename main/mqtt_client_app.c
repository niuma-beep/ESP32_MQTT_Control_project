#include "mqtt_client_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "app_state.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "rgb_led.h"

static const char *TAG = "mqtt_app";

#define MQTT_DEVICE_ID_LEN 32
#define MQTT_BASE_TOPIC_LEN 128
#define MQTT_TOPIC_LEN 160
#define MQTT_LWT_MSG_LEN 96

static esp_mqtt_client_handle_t s_mqtt_client;
static volatile bool s_mqtt_connected;
static volatile bool s_mqtt_started;

static char s_mqtt_device_id[MQTT_DEVICE_ID_LEN];
static char s_mqtt_base_topic[MQTT_BASE_TOPIC_LEN];
static char s_mqtt_status_topic[MQTT_TOPIC_LEN];
static char s_mqtt_telemetry_topic[MQTT_TOPIC_LEN];
static char s_mqtt_cmd_topic[MQTT_TOPIC_LEN];
static char s_mqtt_rgb_cmd_topic[MQTT_TOPIC_LEN];
static char s_mqtt_rgb_state_topic[MQTT_TOPIC_LEN];
static char s_mqtt_lwt_msg[MQTT_LWT_MSG_LEN];

static void mqtt_identity_init(void)
{
  if (s_mqtt_device_id[0] != '\0')
  {
    return;
  }

  uint8_t mac[6] = {0};
  // 读取Esp32的MAC地址作为设备ID的一部分，确保每个设备都有唯一的标识符
  esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "read STA MAC failed: %s", esp_err_to_name(err));
  }
  // 生成设备ID
  snprintf(s_mqtt_device_id, sizeof(s_mqtt_device_id),
           "esp32s3-%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  // 生成MQTT基础主题
  snprintf(s_mqtt_base_topic, sizeof(s_mqtt_base_topic), "%s/%s",
           MQTT_TOPIC_PREFIX, s_mqtt_device_id);
  // 生成MQTT状态主题、遥测主题、命令主题和RGB灯状态主题
  snprintf(s_mqtt_status_topic, sizeof(s_mqtt_status_topic), "%s/status",
           s_mqtt_base_topic);
  snprintf(s_mqtt_telemetry_topic, sizeof(s_mqtt_telemetry_topic),
           "%s/telemetry", s_mqtt_base_topic);
  snprintf(s_mqtt_cmd_topic, sizeof(s_mqtt_cmd_topic), "%s/cmd/#",
           s_mqtt_base_topic);
  snprintf(s_mqtt_rgb_cmd_topic, sizeof(s_mqtt_rgb_cmd_topic), "%s/cmd/rgb",
           s_mqtt_base_topic);
  snprintf(s_mqtt_rgb_state_topic, sizeof(s_mqtt_rgb_state_topic),
           "%s/state/rgb", s_mqtt_base_topic);
  // 生成遗嘱消息，表示设备离线时的状态
  snprintf(s_mqtt_lwt_msg, sizeof(s_mqtt_lwt_msg),
           "{\"device\":\"%s\",\"online\":false}", s_mqtt_device_id);

  ESP_LOGI(TAG, "device_id=%s base_topic=%s", s_mqtt_device_id,
           s_mqtt_base_topic);
}

static uint8_t clamp_color_value(double value)
{
  if (value < 0)
  {
    return 0;
  }
  if (value > 255)
  {
    return 255;
  }
  return (uint8_t)value;
}

static bool parse_hex_digit(char ch, uint8_t *value)
{
  if (ch >= '0' && ch <= '9')
  {
    *value = (uint8_t)(ch - '0');
    return true;
  }
  if (ch >= 'a' && ch <= 'f')
  {
    *value = (uint8_t)(ch - 'a' + 10);
    return true;
  }
  if (ch >= 'A' && ch <= 'F')
  {
    *value = (uint8_t)(ch - 'A' + 10);
    return true;
  }
  return false;
}

static bool parse_hex_color(const char *hex, uint8_t *red, uint8_t *green,
                            uint8_t *blue)
{
  if (!hex)
  {
    return false;
  }

  if (hex[0] == '#')
  {
    hex++;
  }

  if (strlen(hex) != 6)
  {
    return false;
  }

  uint8_t n[6];
  for (int i = 0; i < 6; i++)
  {
    if (!parse_hex_digit(hex[i], &n[i]))
    {
      return false;
    }
  }

  *red = (uint8_t)((n[0] << 4) | n[1]);
  *green = (uint8_t)((n[2] << 4) | n[3]);
  *blue = (uint8_t)((n[4] << 4) | n[5]);
  return true;
}

static bool parse_named_color(const char *name, uint8_t *red, uint8_t *green,
                              uint8_t *blue)
{
  if (!name)
  {
    return false;
  }

  if (strcasecmp(name, "off") == 0 || strcasecmp(name, "black") == 0)
  {
    *red = 0;
    *green = 0;
    *blue = 0;
  }
  else if (strcasecmp(name, "red") == 0)
  {
    *red = 255;
    *green = 0;
    *blue = 0;
  }
  else if (strcasecmp(name, "green") == 0)
  {
    *red = 0;
    *green = 255;
    *blue = 0;
  }
  else if (strcasecmp(name, "blue") == 0)
  {
    *red = 0;
    *green = 0;
    *blue = 255;
  }
  else if (strcasecmp(name, "white") == 0)
  {
    *red = 255;
    *green = 255;
    *blue = 255;
  }
  else if (strcasecmp(name, "warm") == 0)
  {
    *red = 255;
    *green = 160;
    *blue = 60;
  }
  else if (strcasecmp(name, "cyan") == 0)
  {
    *red = 0;
    *green = 255;
    *blue = 255;
  }
  else if (strcasecmp(name, "magenta") == 0)
  {
    *red = 255;
    *green = 0;
    *blue = 255;
  }
  else if (strcasecmp(name, "yellow") == 0)
  {
    *red = 255;
    *green = 220;
    *blue = 0;
  }
  else
  {
    return false;
  }

  return true;
}

static bool topic_equals(const esp_mqtt_event_handle_t event, const char *topic)
{
  int topic_len = (int)strlen(topic);
  return event->topic_len == topic_len &&
         memcmp(event->topic, topic, topic_len) == 0;
}

static char *build_telemetry_json(void)
{
  app_data_t snap;
  uint8_t red;
  uint8_t green;
  uint8_t blue;

  app_data_snapshot(&snap);
  rgb_led_get_color(&red, &green, &blue);

  int64_t now_us = esp_timer_get_time();
  int64_t sensor_age_ms = (now_us - snap.sensor_update_us) / 1000;
  int64_t weather_age_ms = (now_us - snap.weather_update_us) / 1000;
  int64_t wifi_age_ms = (now_us - snap.wifi_update_us) / 1000;

  cJSON *root = cJSON_CreateObject();
  if (!root)
  {
    return NULL;
  }

  cJSON_AddStringToObject(root, "device", s_mqtt_device_id);
  cJSON_AddBoolToObject(root, "sensor_ok", snap.sensor_ok);
  cJSON_AddNumberToObject(root, "sensor_temp", snap.sensor_temp);
  cJSON_AddNumberToObject(root, "sensor_humidity", snap.sensor_humidity);
  cJSON_AddNumberToObject(root, "sensor_age", (double)sensor_age_ms);
  cJSON_AddNumberToObject(root, "sensor_seq", snap.sensor_seq);
  cJSON_AddBoolToObject(root, "weather_ok", snap.weather_ok);
  cJSON_AddStringToObject(root, "weather_temp", snap.weather_temp);
  cJSON_AddStringToObject(root, "weather_humidity", snap.weather_humidity);
  cJSON_AddStringToObject(root, "weather_desc", snap.weather_desc);
  cJSON_AddStringToObject(root, "weather_status", snap.weather_status);
  cJSON_AddStringToObject(root, "wind_dir", snap.wind_dir);
  cJSON_AddStringToObject(root, "wind_scale", snap.wind_scale);
  cJSON_AddStringToObject(root, "weather_time", snap.weather_time);
  cJSON_AddNumberToObject(root, "weather_age", (double)weather_age_ms);
  cJSON_AddNumberToObject(root, "weather_seq", snap.weather_seq);
  cJSON_AddBoolToObject(root, "sta_connected", snap.sta_connected);
  cJSON_AddStringToObject(root, "sta_ip", snap.sta_ip);
  cJSON_AddNumberToObject(root, "wifi_rssi", snap.wifi_rssi);
  cJSON_AddNumberToObject(root, "wifi_age", (double)wifi_age_ms);
  cJSON_AddStringToObject(root, "city", snap.city);
  cJSON_AddNumberToObject(root, "uptime", (double)(now_us / 1000));
  cJSON_AddNumberToObject(root, "free_heap",
                          heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(root, "rgb_r", red);
  cJSON_AddNumberToObject(root, "rgb_g", green);
  cJSON_AddNumberToObject(root, "rgb_b", blue);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

static bool mqtt_send_json(const char *topic, const char *json, int qos,
                           int retain, bool store)
{
  if (!s_mqtt_client)
  {
    return false;
  }

  int msg_id = esp_mqtt_client_enqueue(s_mqtt_client, topic, json, 0, qos,
                                       retain, store);
  if (msg_id < 0)
  {
    ESP_LOGW(TAG, "publish failed topic=%s msg_id=%d outbox=%d",
             topic, msg_id, esp_mqtt_client_get_outbox_size(s_mqtt_client));
    return false;
  }

  return true;
}

static char *build_status_json(bool online)
{
  cJSON *root = cJSON_CreateObject();
  if (!root)
  {
    return NULL;
  }

  cJSON_AddStringToObject(root, "device", s_mqtt_device_id);
  cJSON_AddBoolToObject(root, "online", online);
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

static char *build_rgb_json(void)
{
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  rgb_led_get_color(&red, &green, &blue);

  cJSON *root = cJSON_CreateObject();
  if (!root)
  {
    return NULL;
  }

  cJSON_AddStringToObject(root, "device", s_mqtt_device_id);
  cJSON_AddNumberToObject(root, "r", red);
  cJSON_AddNumberToObject(root, "g", green);
  cJSON_AddNumberToObject(root, "b", blue);
  cJSON_AddBoolToObject(root, "on", red || green || blue);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}
/**
 * @brief 发布当前设备状态
 *
 * @param online
 */
static void publish_status(bool online)
{
  if (!s_mqtt_client || !s_mqtt_connected)
  {
    return;
  }

  char *json = build_status_json(online);
  if (!json)
  {
    return;
  }

  mqtt_send_json(s_mqtt_status_topic, json, 1, 1, false);
  cJSON_free(json);
}
/**
 * @brief 发布RGB灯状态
 *
 */
static void publish_rgb_state(void)
{
  if (!s_mqtt_client || !s_mqtt_connected)
  {
    return;
  }

  char *json = build_rgb_json();
  if (!json)
  {
    return;
  }

  mqtt_send_json(s_mqtt_rgb_state_topic, json, 1, 1, false);
  cJSON_free(json);
}
/**
 * @brief 发布传感器数据和设备状态
 *
 */
static void publish_telemetry(void)
{
  if (!s_mqtt_client || !s_mqtt_connected)
  {
    return;
  }

  char *json = build_telemetry_json();
  if (!json)
  {
    ESP_LOGW(TAG, "telemetry JSON build failed");
    return;
  }

  int msg_id = esp_mqtt_client_enqueue(s_mqtt_client, s_mqtt_telemetry_topic,
                                       json, 0, 1, 0, false);
  if (msg_id < 0)
  {
    ESP_LOGW(TAG, "telemetry enqueue failed msg_id=%d outbox=%d", msg_id,
             esp_mqtt_client_get_outbox_size(s_mqtt_client));
  }
  else
  {
    ESP_LOGI(TAG, "queued telemetry msg_id=%d", msg_id);
  }

  cJSON_free(json);
}

static bool json_get_color(const cJSON *root, const char *name,
                           uint8_t *value)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!cJSON_IsNumber(item))
  {
    return false;
  }

  *value = clamp_color_value(item->valuedouble);
  return true;
}

static bool handle_rgb_command_payload(const char *payload)
{
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  bool have_color = false;

  rgb_led_get_color(&red, &green, &blue);

  cJSON *root = cJSON_Parse(payload);
  if (!root)
  {
    ESP_LOGW(TAG, "invalid RGB JSON: %s", payload);
    return false;
  }

  const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
  if (cJSON_IsString(state) && state->valuestring)
  {
    if (strcasecmp(state->valuestring, "off") == 0)
    {
      red = 0;
      green = 0;
      blue = 0;
      have_color = true;
    }
    else if (strcasecmp(state->valuestring, "on") == 0 &&
             red == 0 && green == 0 && blue == 0)
    {
      red = 64;
      green = 64;
      blue = 64;
      have_color = true;
    }
  }

  const cJSON *color = cJSON_GetObjectItemCaseSensitive(root, "color");
  if (cJSON_IsString(color) && color->valuestring &&
      parse_named_color(color->valuestring, &red, &green, &blue))
  {
    have_color = true;
  }

  const cJSON *hex = cJSON_GetObjectItemCaseSensitive(root, "hex");
  if (cJSON_IsString(hex) && hex->valuestring &&
      parse_hex_color(hex->valuestring, &red, &green, &blue))
  {
    have_color = true;
  }

  uint8_t new_red = red;
  uint8_t new_green = green;
  uint8_t new_blue = blue;
  bool have_rgb = json_get_color(root, "r", &new_red) |
                  json_get_color(root, "g", &new_green) |
                  json_get_color(root, "b", &new_blue);
  if (have_rgb)
  {
    red = new_red;
    green = new_green;
    blue = new_blue;
    have_color = true;
  }

  cJSON_Delete(root);

  if (!have_color)
  {
    ESP_LOGW(TAG, "RGB command has no supported color fields: %s", payload);
    return false;
  }

  rgb_led_set_color(red, green, blue);
  ESP_LOGI(TAG, "RGB command applied r=%u g=%u b=%u", red, green, blue);
  publish_rgb_state();
  publish_telemetry();
  return true;
}
/**
 * @brief MQTT命令处理函数，处理服务器下发的控制命令
 *
 * @param event
 */
static void handle_command(const esp_mqtt_event_handle_t event)
{
  char topic[96] = {0};
  char payload[256] = {0};
  int topic_len = event->topic_len < (int)sizeof(topic) - 1
                      ? event->topic_len
                      : (int)sizeof(topic) - 1;
  int data_len = event->data_len < (int)sizeof(payload) - 1
                     ? event->data_len
                     : (int)sizeof(payload) - 1;

  memcpy(topic, event->topic, topic_len);
  memcpy(payload, event->data, data_len);
  ESP_LOGI(TAG, "command topic=%s payload=%s", topic, payload);

  if (event->data_len >= (int)sizeof(payload))
  {
    ESP_LOGW(TAG, "command payload too large");
    return;
  }

  if (topic_equals(event, s_mqtt_rgb_cmd_topic))
  {
    handle_rgb_command_payload(payload);
  }
}
/**
 * @brief MQTT事件处理函数
 *
 * @param handler_args
 * @param base
 * @param event_id
 * @param event_data
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
  (void)handler_args;
  (void)base;

  esp_mqtt_event_handle_t event = event_data;
  switch ((esp_mqtt_event_id_t)event_id)
  {
  case MQTT_EVENT_CONNECTED: // MQTT连接成功事件
    s_mqtt_connected = true;
    ESP_LOGI(TAG, "connected to %s", MQTT_BROKER_URI);
    publish_status(true);
    publish_rgb_state();
    esp_mqtt_client_subscribe(s_mqtt_client, s_mqtt_cmd_topic, 1);
    publish_telemetry();
    break;

  case MQTT_EVENT_DISCONNECTED: // MQTT断开连接事件
    s_mqtt_connected = false;
    ESP_LOGW(TAG, "disconnected");
    break;

  case MQTT_EVENT_DATA: // MQTT收到数据事件,服务器给客户端发送消息时触发
    handle_command(event);
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGW(TAG, "MQTT error");
    break;

  default:
    break;
  }
}

static void mqtt_publish_task(void *arg)
{
  (void)arg;

  while (1)
  {
    app_data_t snap;
    app_data_snapshot(&snap);

    if (snap.sta_connected && s_mqtt_connected)
    {
      publish_telemetry();
      vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_INTERVAL_MS));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

static void mqtt_start_when_wifi_ready_task(void *arg)
{
  (void)arg;

  while (1)
  {
    app_data_t snap;
    app_data_snapshot(&snap);

    if (snap.sta_connected)
    {
      ESP_LOGI(TAG, "STA is online, starting MQTT client");
      esp_mqtt_client_start(s_mqtt_client);
      s_mqtt_started = true;
      xTaskCreate(mqtt_publish_task, "mqtt_publish", 6144, NULL, 5, NULL);
      vTaskDelete(NULL);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

const char *mqtt_client_app_get_device_id(void)
{
  mqtt_identity_init();
  return s_mqtt_device_id;
}

const char *mqtt_client_app_get_base_topic(void)
{
  mqtt_identity_init();
  return s_mqtt_base_topic;
}

void mqtt_client_app_start(void)
{
  mqtt_identity_init();

  esp_mqtt_client_config_t mqtt_cfg = {
      .broker.address.uri = MQTT_BROKER_URI,
      .credentials.client_id = s_mqtt_device_id,
      .session.keepalive = 60,
      .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
      // 初始化遗嘱消息
      .session.last_will.topic = s_mqtt_status_topic,
      .session.last_will.msg = s_mqtt_lwt_msg,
      .session.last_will.qos = 1,
      .session.last_will.retain = 1,
      // 设置重连时间和超时时间
      .network.reconnect_timeout_ms = 3000,
      .network.timeout_ms = 20000,
      .task.stack_size = 6144,
  };

  s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  if (!s_mqtt_client)
  {
    ESP_LOGE(TAG, "client init failed");
    return;
  }

  esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, NULL);
  if (!s_mqtt_started)
  {
    xTaskCreate(mqtt_start_when_wifi_ready_task, "mqtt_start", 4096, NULL, 5, NULL);
  }
}
