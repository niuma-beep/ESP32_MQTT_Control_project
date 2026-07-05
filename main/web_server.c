#include "web_server.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "app_state.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client_app.h"

static const char *TAG = "web_server";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

typedef struct
{
  char path[64];
  char query[128];
} request_url_t;

static void parse_request_url(const char *uri, request_url_t *url)
{
  const char *query = strchr(uri, '?');

  memset(url, 0, sizeof(*url));
  if (query)
  {
    size_t path_len = query - uri;
    if (path_len >= sizeof(url->path))
    {
      path_len = sizeof(url->path) - 1;
    }
    memcpy(url->path, uri, path_len);
    url->path[path_len] = '\0';
    strlcpy(url->query, query + 1, sizeof(url->query));
  }
  else
  {
    strlcpy(url->path, uri, sizeof(url->path));
  }
}

static void log_request_url(httpd_req_t *req, const request_url_t *url)
{
  char host[64] = "";
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
  {
    strlcpy(host, "-", sizeof(host));
  }

  ESP_LOGI(TAG, "HTTP %s host=%s path=%s query=%s",
           http_method_str(req->method), host, url->path,
           url->query[0] ? url->query : "-");
}

static esp_err_t discard_request_body(httpd_req_t *req)
{
  char buf[64];
  int remaining = req->content_len;

  while (remaining > 0)
  {
    int recv_len = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
    if (recv_len <= 0)
    {
      return ESP_FAIL;
    }
    remaining -= recv_len;
  }
  return ESP_OK;
}

static cJSON *create_data_json(void)
{
  app_data_t snap;
  app_data_snapshot(&snap);

  int64_t now_us = esp_timer_get_time();
  int64_t sensor_age_ms = (now_us - snap.sensor_update_us) / 1000;
  int64_t weather_age_ms = (now_us - snap.weather_update_us) / 1000;
  int64_t wifi_age_ms = (now_us - snap.wifi_update_us) / 1000;
  int64_t uptime_ms = now_us / 1000;

  cJSON *root = cJSON_CreateObject();
  if (!root)
  {
    return NULL;
  }

  cJSON_AddBoolToObject(root, "sensor_ok", snap.sensor_ok);
  cJSON_AddNumberToObject(root, "sensor_temp", snap.sensor_temp);
  cJSON_AddNumberToObject(root, "sensor_humidity", snap.sensor_humidity);
  cJSON_AddNumberToObject(root, "sensor_age", (double)sensor_age_ms);
  cJSON_AddNumberToObject(root, "sensor_seq", snap.sensor_seq);
  cJSON_AddBoolToObject(root, "weather_ok", snap.weather_ok);
  cJSON_AddStringToObject(root, "weather_temp", snap.weather_temp);
  cJSON_AddStringToObject(root, "weather_humidity", snap.weather_humidity);
  cJSON_AddStringToObject(root, "weather_desc", snap.weather_desc);
  cJSON_AddStringToObject(root, "wind_dir", snap.wind_dir);
  cJSON_AddStringToObject(root, "wind_scale", snap.wind_scale);
  cJSON_AddStringToObject(root, "weather_time", snap.weather_time);
  cJSON_AddStringToObject(root, "weather_status", snap.weather_status);
  cJSON_AddNumberToObject(root, "weather_age", (double)weather_age_ms);
  cJSON_AddNumberToObject(root, "weather_seq", snap.weather_seq);
  cJSON_AddNumberToObject(root, "wifi_age", (double)wifi_age_ms);
  cJSON_AddBoolToObject(root, "sta_connected", snap.sta_connected);
  cJSON_AddStringToObject(root, "sta_ip", snap.sta_ip);
  cJSON_AddNumberToObject(root, "wifi_rssi", snap.wifi_rssi);
  cJSON_AddStringToObject(root, "city", snap.city);
  cJSON_AddNumberToObject(root, "uptime", (double)uptime_ms);
  cJSON_AddNumberToObject(root, "free_heap",
                          heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  cJSON_AddStringToObject(root, "chip", "ESP32-S3");
  cJSON_AddStringToObject(root, "device_id",
                          mqtt_client_app_get_device_id());
  cJSON_AddStringToObject(root, "mqtt_base_topic",
                          mqtt_client_app_get_base_topic());

  return root;
}

static esp_err_t send_data_json(httpd_req_t *req)
{
  cJSON *root = create_data_json();
  char *json = root ? cJSON_PrintUnformatted(root) : NULL;
  if (!json)
  {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "JSON build failed");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  cJSON_free(json);
  cJSON_Delete(root);
  return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
  request_url_t url;
  parse_request_url(req->uri, &url);
  log_request_url(req, &url);

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, index_html_start,
                  index_html_end - index_html_start);
  return ESP_OK;
}

static esp_err_t api_data_post_handler(httpd_req_t *req)
{
  request_url_t url;
  parse_request_url(req->uri, &url);
  log_request_url(req, &url);

  if (discard_request_body(req) != ESP_OK)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    return ESP_FAIL;
  }

  return send_data_json(req);
}

static esp_err_t api_data_get_handler(httpd_req_t *req)
{
  request_url_t url;
  parse_request_url(req->uri, &url);
  log_request_url(req, &url);

  httpd_resp_set_status(req, "405 Method Not Allowed");
  httpd_resp_set_hdr(req, "Allow", "POST");
  httpd_resp_send(req, "Use POST /api/data", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
  (void)err;

  request_url_t url;
  parse_request_url(req->uri, &url);
  log_request_url(req, &url);

  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

void start_webserver(void)
{
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;

  ESP_ERROR_CHECK(httpd_start(&server, &config));

  httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = root_get_handler,
  };
  httpd_uri_t api_post = {
      .uri = "/api/data",
      .method = HTTP_POST,
      .handler = api_data_post_handler,
  };
  httpd_uri_t api_get = {
      .uri = "/api/data",
      .method = HTTP_GET,
      .handler = api_data_get_handler,
  };

  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api_post));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api_get));
  ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND,
                                             http_404_handler));
}
