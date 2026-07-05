#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct
{
  bool sensor_ok;
  float sensor_temp;
  float sensor_humidity;
  uint64_t sensor_update_us;
  uint32_t sensor_seq;
  bool weather_ok;
  bool sta_connected;
  uint64_t weather_update_us;
  uint32_t weather_seq;
  uint64_t wifi_update_us;
  char sta_ip[16];
  int wifi_rssi;
  char city[32];
  char weather_temp[16];
  char weather_humidity[16];
  char weather_desc[64];
  char wind_dir[32];
  char wind_scale[32];
  char weather_time[24];
  char weather_status[64];
} app_data_t;

extern app_data_t s_data;
extern SemaphoreHandle_t s_data_lock;

void app_state_init(void);
void app_data_snapshot(app_data_t *snapshot);
void app_data_update_sensor(float temp, float humidity, bool ok);
