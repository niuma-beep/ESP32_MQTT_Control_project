#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t voice_control_start(void);
esp_err_t voice_control_stop_and_execute(void);
void voice_control_cancel(void);
bool voice_control_is_recording(void);