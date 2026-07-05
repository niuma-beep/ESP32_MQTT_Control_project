#pragma once

#include "esp_netif.h"

void wifi_init_apsta(esp_netif_t **ap_netif_out);
void set_captive_portal_options(esp_netif_t *ap_netif);
