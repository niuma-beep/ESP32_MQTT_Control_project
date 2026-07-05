#include "rgb_led.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rgb_led";

static portMUX_TYPE s_rgb_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_red;
static uint8_t s_green;
static uint8_t s_blue;
static bool s_dirty = true;

static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 3,
    .level1 = 0,
    .duration1 = 9,
};

static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 9,
    .level1 = 0,
    .duration1 = 3,
};

static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = 250,
    .level1 = 0,
    .duration1 = 250,
};

static size_t ws2812_encoder_callback(const void *data, size_t data_size,
                                      size_t symbols_written,
                                      size_t symbols_free,
                                      rmt_symbol_word_t *symbols, bool *done,
                                      void *arg)
{
  (void)arg;

  if (symbols_free < 8)
  {
    return 0;
  }

  const uint8_t *data_bytes = data;
  size_t data_pos = symbols_written / 8;

  if (data_pos < data_size)
  {
    size_t symbol_pos = 0;
    for (uint8_t bitmask = 0x80; bitmask != 0; bitmask >>= 1)
    {
      symbols[symbol_pos++] =
          (data_bytes[data_pos] & bitmask) ? ws2812_one : ws2812_zero;
    }
    return symbol_pos;
  }

  symbols[0] = ws2812_reset;
  *done = true;
  return 1;
}

static void ws2812_set_rgb(rmt_channel_handle_t led_chan,
                           rmt_encoder_handle_t led_encoder, uint8_t red,
                           uint8_t green, uint8_t blue)
{
  uint8_t led_data[RGB_LED_NUMBERS * 3] = {
      green,
      red,
      blue,
  };
  rmt_transmit_config_t tx_config = {
      .loop_count = 0,
  };

  ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_data,
                               sizeof(led_data), &tx_config));
  ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}

static bool rgb_led_take_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
  bool dirty;

  taskENTER_CRITICAL(&s_rgb_mux);
  *red = s_red;
  *green = s_green;
  *blue = s_blue;
  dirty = s_dirty;
  s_dirty = false;
  taskEXIT_CRITICAL(&s_rgb_mux);

  return dirty;
}

void rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
  taskENTER_CRITICAL(&s_rgb_mux);
  s_red = red;
  s_green = green;
  s_blue = blue;
  s_dirty = true;
  taskEXIT_CRITICAL(&s_rgb_mux);
}

void rgb_led_get_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
  uint8_t r;
  uint8_t g;
  uint8_t b;

  taskENTER_CRITICAL(&s_rgb_mux);
  r = s_red;
  g = s_green;
  b = s_blue;
  taskEXIT_CRITICAL(&s_rgb_mux);

  if (red)
  {
    *red = r;
  }
  if (green)
  {
    *green = g;
  }
  if (blue)
  {
    *blue = b;
  }
}

void led_task(void *arg)
{
  (void)arg;
  ESP_LOGI(TAG, "init WS2812 RGB LED on GPIO%d", RGB_LED_GPIO);

  rmt_channel_handle_t led_chan = NULL;
  rmt_tx_channel_config_t tx_chan_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .gpio_num = RGB_LED_GPIO,
      .mem_block_symbols = 64,
      .resolution_hz = RMT_LED_RESOLUTION_HZ,
      .trans_queue_depth = 4,
  };
  ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

  rmt_encoder_handle_t led_encoder = NULL;
  rmt_simple_encoder_config_t encoder_config = {
      .callback = ws2812_encoder_callback,
  };
  ESP_ERROR_CHECK(rmt_new_simple_encoder(&encoder_config, &led_encoder));
  ESP_ERROR_CHECK(rmt_enable(led_chan));

  while (1)
  {
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    if (rgb_led_take_color(&red, &green, &blue))
    {
      ws2812_set_rgb(led_chan, led_encoder, red, green, blue);
      ESP_LOGI(TAG, "RGB set to r=%u g=%u b=%u", red, green, blue);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}