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
#include "lwip/inet.h"

static const char *TAG = "wifi_portal";
static char s_captive_uri[32];

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
    esp_wifi_connect();
  }
  else if (event_base == WIFI_EVENT &&
           event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    xSemaphoreTake(s_data_lock, portMAX_DELAY);
    s_data.sta_connected = false;
    s_data.wifi_update_us = esp_timer_get_time();
    s_data.sta_ip[0] = '\0';
    s_data.wifi_rssi = -127;
    xSemaphoreGive(s_data_lock);
    esp_wifi_connect();
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
    }
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

  wifi_config_t sta_config = {
      .sta = {
          .ssid = ROUTER_WIFI_SSID,
          .password = ROUTER_WIFI_PASS,
          .threshold.authmode = WIFI_AUTH_WPA2_PSK,
          .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
      },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "AP SSID:%s password:%s", PORTAL_AP_SSID, PORTAL_AP_PASS);
  *ap_netif_out = ap_netif;
}
