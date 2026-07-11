#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_netif.h"

void wifi_init_apsta(esp_netif_t **ap_netif_out);
void set_captive_portal_options(esp_netif_t *ap_netif);
cJSON *wifi_portal_create_scan_json(esp_err_t *out_err);
esp_err_t wifi_portal_connect_sta(const char *ssid, const char *password);
void wifi_portal_get_sta_ssid(char *ssid, size_t ssid_size);
