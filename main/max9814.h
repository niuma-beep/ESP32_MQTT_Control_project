#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct
{
  int raw_min;
  int raw_max;
  int raw_avg;
  int peak_to_peak;
  uint8_t level;
} max9814_sample_t;

esp_err_t max9814_init(void);
esp_err_t max9814_read_raw(int *raw);
esp_err_t max9814_sample(max9814_sample_t *sample, uint32_t window_ms);
void max9814_task(void *arg);
