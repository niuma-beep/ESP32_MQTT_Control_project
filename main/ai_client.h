#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
  AI_COMMAND_NONE = 0,
  AI_COMMAND_SET_BRIGHTNESS,
  AI_COMMAND_SET_DISPLAY_MODE,
  AI_COMMAND_SET_EFFECT,
  AI_COMMAND_SET_COLOR,
} ai_command_type_t;

typedef struct
{
  ai_command_type_t type;
  int value;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  char text[24];
} ai_command_t;

esp_err_t ai_client_send_audio(const int16_t *pcm, size_t sample_count,
                               uint32_t sample_rate_hz, ai_command_t *command);