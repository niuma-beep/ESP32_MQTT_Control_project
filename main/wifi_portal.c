#include "wifi_portal.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_state.h"
#include "app_time.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"

static const char *TAG = "wifi_portal";
static char s_captive_uri[32];
static bool s_sta_has_config = false;
static bool s_sta_connecting = false;
static esp_timer_handle_t s_sta_connect_timer;
static portMUX_TYPE s_sta_conn_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_sta_info_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_sta_ssid[33];

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASS_KEY "pass"
#define WIFI_SCAN_MAX_AP 20
#define WIFI_CONNECT_TIMEOUT_MS 15000

static const char *wifi_authmode_str(wifi_auth_mode_t authmode)
{
  switch (authmode)
  {
  case WIFI_AUTH_OPEN:
    return "OPEN";
  case WIFI_AUTH_WEP:
    return "WEP";
  case WIFI_AUTH_WPA_PSK:
    return "WPA";
  case WIFI_AUTH_WPA2_PSK:
    return "WPA2";
  case WIFI_AUTH_WPA_WPA2_PSK:
    return "WPA/WPA2";
  case WIFI_AUTH_WPA2_ENTERPRISE:
    return "WPA2-ENT";
  case WIFI_AUTH_WPA3_PSK:
    return "WPA3";
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return "WPA2/WPA3";
  case WIFI_AUTH_WAPI_PSK:
    return "WAPI";
  default:
    return "UNKNOWN";
  }
}

static void set_sta_disconnected_state(void)
{
  taskENTER_CRITICAL(&s_sta_info_mux);
  s_sta_ssid[0] = '\0';
  taskEXIT_CRITICAL(&s_sta_info_mux);

  xSemaphoreTake(s_data_lock, portMAX_DELAY);
  s_data.sta_connected = false;
  s_data.wifi_update_us = esp_timer_get_time();
  s_data.sta_ip[0] = '\0';
  s_data.wifi_rssi = -127;
  xSemaphoreGive(s_data_lock);
}

static void set_sta_has_config(bool has_config)
{
  taskENTER_CRITICAL(&s_sta_conn_mux);
  s_sta_has_config = has_config;
  taskEXIT_CRITICAL(&s_sta_conn_mux);
}

static bool sta_has_config(void)
{
  bool has_config;

  taskENTER_CRITICAL(&s_sta_conn_mux);
  has_config = s_sta_has_config;
  taskEXIT_CRITICAL(&s_sta_conn_mux);

  return has_config;
}

static bool sta_is_connecting(void)
{
  bool connecting;

  taskENTER_CRITICAL(&s_sta_conn_mux);
  connecting = s_sta_connecting;
  taskEXIT_CRITICAL(&s_sta_conn_mux);

  return connecting;
}

static void stop_sta_connect_window(bool keep_config)
{
  taskENTER_CRITICAL(&s_sta_conn_mux);
  s_sta_connecting = false;
  if (!keep_config)
  {
    s_sta_has_config = false;
  }
  taskEXIT_CRITICAL(&s_sta_conn_mux);

  if (s_sta_connect_timer)
  {
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(s_sta_connect_timer));
  }
}

static void start_sta_connect_window(void)
{
  bool already_connecting;

  taskENTER_CRITICAL(&s_sta_conn_mux);
  already_connecting = s_sta_connecting;
  s_sta_connecting = true;
  taskEXIT_CRITICAL(&s_sta_conn_mux);

  if (!already_connecting && s_sta_connect_timer)
  {
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(s_sta_connect_timer));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_timer_start_once(s_sta_connect_timer,
                             WIFI_CONNECT_TIMEOUT_MS * 1000ULL));
  }
}

static void sta_connect_timeout_cb(void *arg)
{
  (void)arg;

  if (!sta_is_connecting())
  {
    return;
  }

  ESP_LOGW(TAG, "STA connect timeout after %d ms, pause reconnect",
           WIFI_CONNECT_TIMEOUT_MS);
  taskENTER_CRITICAL(&s_sta_conn_mux);
  s_sta_connecting = false;
  s_sta_has_config = false;
  taskEXIT_CRITICAL(&s_sta_conn_mux);
  set_sta_disconnected_state();
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
}

static void sta_connect_with_timeout(void)
{
  start_sta_connect_window();
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
  }
}

static void pause_sta_connecting_for_scan(void)
{
  if (!sta_is_connecting())
  {
    return;
  }

  ESP_LOGI(TAG, "pause STA connecting before WiFi scan");
  stop_sta_connect_window(false);
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
  set_sta_disconnected_state();
  vTaskDelay(pdMS_TO_TICKS(250));
}

static bool has_ssid(const wifi_config_t *config)
{
  return config->sta.ssid[0] != '\0';
}

static void copy_wifi_field(uint8_t *dst, size_t dst_size, const char *src)
{
  size_t len = strlen(src);
  if (len > dst_size)
  {
    len = dst_size;
  }
  memset(dst, 0, dst_size);
  memcpy(dst, src, len);
}

static void set_current_sta_ssid(const uint8_t *ssid)
{
  char tmp[sizeof(s_sta_ssid)];
  size_t len = strnlen((const char *)ssid, sizeof(tmp) - 1);
  memcpy(tmp, ssid, len);
  tmp[len] = '\0';

  taskENTER_CRITICAL(&s_sta_info_mux);
  strlcpy(s_sta_ssid, tmp, sizeof(s_sta_ssid));
  taskEXIT_CRITICAL(&s_sta_info_mux);
}

static void load_sta_config(wifi_config_t *sta_config)
{
  memset(sta_config, 0, sizeof(*sta_config));
  copy_wifi_field(sta_config->sta.ssid, sizeof(sta_config->sta.ssid),
                  ROUTER_WIFI_SSID);
  copy_wifi_field(sta_config->sta.password, sizeof(sta_config->sta.password),
                  ROUTER_WIFI_PASS);
  sta_config->sta.threshold.authmode = WIFI_AUTH_OPEN;
  sta_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK)
  {
    ESP_LOGI(TAG, "using default router WiFi config");
    return;
  }

  char ssid[33] = "";
  char password[65] = "";
  size_t ssid_len = sizeof(ssid);
  size_t pass_len = sizeof(password);

  err = nvs_get_str(nvs, WIFI_NVS_SSID_KEY, ssid, &ssid_len);
  if (err == ESP_OK && ssid[0] != '\0')
  {
    if (nvs_get_str(nvs, WIFI_NVS_PASS_KEY, password, &pass_len) != ESP_OK)
    {
      password[0] = '\0';
    }
    copy_wifi_field(sta_config->sta.ssid, sizeof(sta_config->sta.ssid), ssid);
    copy_wifi_field(sta_config->sta.password, sizeof(sta_config->sta.password),
                    password);
    ESP_LOGI(TAG, "loaded router WiFi config from NVS: %s", ssid);
  }
  else
  {
    ESP_LOGI(TAG, "no saved router WiFi config, using defaults");
  }

  nvs_close(nvs);
}

static esp_err_t save_sta_config(const char *ssid, const char *password)
{
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK)
  {
    return err;
  }

  err = nvs_set_str(nvs, WIFI_NVS_SSID_KEY, ssid);
  if (err == ESP_OK)
  {
    err = nvs_set_str(nvs, WIFI_NVS_PASS_KEY, password ? password : "");
  }
  if (err == ESP_OK)
  {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);
  return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
  (void)arg;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
  {
    wifi_event_ap_staconnected_t *event = event_data;
    ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d",
             MAC2STR(event->mac), event->aid);
  }
  else if (event_base == WIFI_EVENT &&
           event_id == WIFI_EVENT_AP_STADISCONNECTED)
  {
    wifi_event_ap_stadisconnected_t *event = event_data;
    ESP_LOGI(TAG, "station " MACSTR " left, reason=%d",
             MAC2STR(event->mac), event->reason);
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    if (sta_has_config())
    {
      sta_connect_with_timeout();
    }
  }
  else if (event_base == WIFI_EVENT &&
           event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    set_sta_disconnected_state();
    if (sta_has_config())
    {
      sta_connect_with_timeout();
    }
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = event_data;
    ESP_LOGI(TAG, "router connected, IP:" IPSTR, IP2STR(&event->ip_info.ip));
    wifi_ap_record_t ap_info;
    int rssi = -127;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
      rssi = ap_info.rssi;
      set_current_sta_ssid(ap_info.ssid);
    }
    stop_sta_connect_window(true);
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    s_data.sta_connected = true;
    s_data.wifi_update_us = esp_timer_get_time();
    inet_ntoa_r(event->ip_info.ip.addr, s_data.sta_ip, sizeof(s_data.sta_ip));
    s_data.wifi_rssi = rssi;
    xSemaphoreGive(s_data_lock);
    start_sntp();
  }
}

void set_captive_portal_options(esp_netif_t *ap_netif)
{
  esp_netif_ip_info_t ip_info;
  ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip_info));

  char ip_addr[16];
  inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));
  snprintf(s_captive_uri, sizeof(s_captive_uri), "http://%s/", ip_addr);

  esp_netif_dns_info_t dns = {
      .ip.type = ESP_IPADDR_TYPE_V4,
  };
  dns.ip.u_addr.ip4.addr = ip_info.ip.addr;

  uint8_t offer_dns = DHCPS_OFFER_DNS;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(ap_netif));
  ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                         ESP_NETIF_DOMAIN_NAME_SERVER,
                                         &offer_dns, sizeof(offer_dns)));
  ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));
  ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                         ESP_NETIF_CAPTIVEPORTAL_URI,
                                         s_captive_uri,
                                         strlen(s_captive_uri)));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(ap_netif));

  ESP_LOGI(TAG, "portal URL: %s", s_captive_uri);
}

void wifi_init_apsta(esp_netif_t **ap_netif_out)
{
  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_timer_create_args_t connect_timer_args = {
      .callback = sta_connect_timeout_cb,
      .name = "sta_connect_timeout",
  };
  ESP_ERROR_CHECK(esp_timer_create(&connect_timer_args,
                                   &s_sta_connect_timer));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             wifi_event_handler, NULL));

  wifi_config_t ap_config = {
      .ap = {
          .ssid = PORTAL_AP_SSID,
          .ssid_len = strlen(PORTAL_AP_SSID),
          .channel = PORTAL_AP_CHANNEL,
          .password = PORTAL_AP_PASS,
          .max_connection = PORTAL_MAX_CONN,
          .authmode = WIFI_AUTH_WPA2_PSK,
          .pmf_cfg = {
              .required = false,
          },
      },
  };
  if (strlen(PORTAL_AP_PASS) == 0)
  {
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  wifi_config_t sta_config;
  load_sta_config(&sta_config);
  set_sta_has_config(has_ssid(&sta_config));
  bool has_sta_config = sta_has_config();

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  if (has_sta_config)
  {
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  }
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "AP SSID:%s password:%s", PORTAL_AP_SSID, PORTAL_AP_PASS);
  ESP_LOGI(TAG, "STA SSID:%s", has_sta_config ? (char *)sta_config.sta.ssid : "-");
  *ap_netif_out = ap_netif;
}

cJSON *wifi_portal_create_scan_json(esp_err_t *out_err)
{
  esp_err_t err = ESP_OK;
  pause_sta_connecting_for_scan();

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = true,
  };

  err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK)
  {
    if (out_err)
    {
      *out_err = err;
    }
    return NULL;
  }

  uint16_t total = 0;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&total));

  wifi_ap_record_t records[WIFI_SCAN_MAX_AP];
  uint16_t count = WIFI_SCAN_MAX_AP;
  err = esp_wifi_scan_get_ap_records(&count, records);
  if (err != ESP_OK)
  {
    if (out_err)
    {
      *out_err = err;
    }
    return NULL;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *networks = root ? cJSON_CreateArray() : NULL;
  if (!root || !networks)
  {
    cJSON_Delete(root);
    if (out_err)
    {
      *out_err = ESP_ERR_NO_MEM;
    }
    return NULL;
  }

  cJSON_AddNumberToObject(root, "total", total);
  cJSON_AddNumberToObject(root, "count", count);
  cJSON_AddItemToObject(root, "networks", networks);

  for (uint16_t i = 0; i < count; ++i)
  {
    char ssid[33];
    size_t ssid_len = strnlen((const char *)records[i].ssid,
                              sizeof(ssid) - 1);
    memcpy(ssid, records[i].ssid, ssid_len);
    ssid[ssid_len] = '\0';

    cJSON *item = cJSON_CreateObject();
    if (!item)
    {
      continue;
    }

    cJSON_AddStringToObject(item, "ssid", ssid);
    cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
    cJSON_AddNumberToObject(item, "channel", records[i].primary);
    cJSON_AddStringToObject(item, "auth",
                            wifi_authmode_str(records[i].authmode));
    cJSON_AddBoolToObject(item, "secure",
                          records[i].authmode != WIFI_AUTH_OPEN);
    cJSON_AddItemToArray(networks, item);
  }

  if (out_err)
  {
    *out_err = ESP_OK;
  }
  return root;
}

esp_err_t wifi_portal_connect_sta(const char *ssid, const char *password)
{
  if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32)
  {
    return ESP_ERR_INVALID_ARG;
  }
  if (password && strlen(password) > 63)
  {
    return ESP_ERR_INVALID_ARG;
  }

  wifi_config_t sta_config = {
      .sta = {
          .threshold.authmode = WIFI_AUTH_OPEN,
          .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
      },
  };
  copy_wifi_field(sta_config.sta.ssid, sizeof(sta_config.sta.ssid), ssid);
  copy_wifi_field(sta_config.sta.password, sizeof(sta_config.sta.password),
                  password ? password : "");

  esp_err_t err = save_sta_config(ssid, password ? password : "");
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "save WiFi config failed: %s", esp_err_to_name(err));
    return err;
  }

  stop_sta_connect_window(false);
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
  err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
  if (err == ESP_OK)
  {
    set_sta_has_config(true);
    set_sta_disconnected_state();
    start_sta_connect_window();
    err = esp_wifi_connect();
  }

  ESP_LOGI(TAG, "connect STA SSID:%s result:%s", ssid, esp_err_to_name(err));
  return err;
}

void wifi_portal_get_sta_ssid(char *ssid, size_t ssid_size)
{
  if (!ssid || ssid_size == 0)
  {
    return;
  }

  taskENTER_CRITICAL(&s_sta_info_mux);
  strlcpy(ssid, s_sta_ssid, ssid_size);
  taskEXIT_CRITICAL(&s_sta_info_mux);
}
