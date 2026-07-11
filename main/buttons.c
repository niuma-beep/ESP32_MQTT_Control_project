#include "buttons.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rgb_led.h"
#include "voice_control.h"

static const char *TAG = "buttons";

#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_RELEASE_LEVEL 1
#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_MS 60

typedef struct
{
  gpio_num_t gpio;
  int stable_level;
  int last_level;
  TickType_t last_change_tick;
  TickType_t press_tick;
} button_state_t;

static button_state_t s_buttons[] = {
    {.gpio = BTN1_GPIO, .stable_level = 1, .last_level = 1},
    {.gpio = BTN2_GPIO, .stable_level = 1, .last_level = 1},
    {.gpio = BTN3_GPIO, .stable_level = 1, .last_level = 1},
};

static const uint8_t s_brightness_steps[] = {8, 16, 32, 64, 102};
static const char *const s_display_modes[] = {
    "time",
    "sensor",
    "weather",
    "waterfall",
    "test",
};
static const char *const s_effects[] = {
    "normal",
    "rainbow",
};

static size_t s_mode_index;
static size_t s_effect_index;

static void configure_button_gpio(gpio_num_t gpio)
{
  gpio_config_t config = {
      .pin_bit_mask = 1ULL << gpio,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&config));
}

static void cycle_brightness(void)
{
  uint8_t current = clock_led_get_brightness();
  uint8_t next = s_brightness_steps[0];

  for (size_t i = 0; i < sizeof(s_brightness_steps); i++)
  {
    if (s_brightness_steps[i] > current)
    {
      next = s_brightness_steps[i];
      break;
    }
  }

  clock_led_set_brightness(next);
  ESP_LOGI(TAG, "brightness -> %u", next);
}

static void cycle_display_mode(void)
{
  s_mode_index = (s_mode_index + 1) %
                 (sizeof(s_display_modes) / sizeof(s_display_modes[0]));
  clock_led_set_mode(s_display_modes[s_mode_index]);
  ESP_LOGI(TAG, "display mode -> %s", s_display_modes[s_mode_index]);
}

static void cycle_effect(void)
{
  s_effect_index = (s_effect_index + 1) %
                   (sizeof(s_effects) / sizeof(s_effects[0]));
  clock_led_set_effect(s_effects[s_effect_index]);
  ESP_LOGI(TAG, "effect -> %s", s_effects[s_effect_index]);
}

static void handle_short_press(size_t index)
{
  switch (index)
  {
  case 0:
    cycle_brightness();
    break;
  case 1:
    cycle_display_mode();
    break;
  case 2:
    cycle_effect();
    break;
  default:
    break;
  }
}

static void handle_button_pressed(size_t index, TickType_t now)
{
  s_buttons[index].press_tick = now;
  if (index == 2)
  {
    esp_err_t ret = voice_control_start();
    if (ret != ESP_OK)
    {
      ESP_LOGW(TAG, "voice start failed: %s", esp_err_to_name(ret));
    }
  }
  else
  {
    handle_short_press(index);
  }
}

static void handle_button_released(size_t index, TickType_t now)
{
  if (index != 2)
  {
    return;
  }

  uint32_t held_ms = pdTICKS_TO_MS(now - s_buttons[index].press_tick);
  if (held_ms < VOICE_TRIGGER_HOLD_MS)
  {
    voice_control_cancel();
    cycle_effect();
    return;
  }

  ESP_LOGI(TAG, "voice trigger, held %lu ms", (unsigned long)held_ms);
  esp_err_t ret = voice_control_stop_and_execute();
  if (ret != ESP_OK)
  {
    ESP_LOGW(TAG, "voice control failed: %s", esp_err_to_name(ret));
  }
}

void buttons_task(void *arg)
{
  (void)arg;

  for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++)
  {
    configure_button_gpio(s_buttons[i].gpio);
    s_buttons[i].stable_level = gpio_get_level(s_buttons[i].gpio);
    s_buttons[i].last_level = s_buttons[i].stable_level;
    s_buttons[i].last_change_tick = xTaskGetTickCount();
  }

  ESP_LOGI(TAG, "init BTN1 GPIO%d BTN2 GPIO%d BTN3 GPIO%d",
           BTN1_GPIO, BTN2_GPIO, BTN3_GPIO);

  while (1)
  {
    TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++)
    {
      int level = gpio_get_level(s_buttons[i].gpio);
      if (level != s_buttons[i].last_level)
      {
        s_buttons[i].last_level = level;
        s_buttons[i].last_change_tick = now;
        continue;
      }

      if (level != s_buttons[i].stable_level &&
          (now - s_buttons[i].last_change_tick) >=
              pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS))
      {
        s_buttons[i].stable_level = level;
        if (level == BUTTON_ACTIVE_LEVEL)
        {
          handle_button_pressed(i, now);
        }
        else if (level == BUTTON_RELEASE_LEVEL)
        {
          handle_button_released(i, now);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}