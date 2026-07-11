#include "rgb_led.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_state.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rgb_led";

static portMUX_TYPE s_rgb_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_rgb_red;
static uint8_t s_rgb_green;
static uint8_t s_rgb_blue;
static bool s_rgb_dirty = true;

static portMUX_TYPE s_clock_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_clock_red = 255;
static uint8_t s_clock_green = 255;
static uint8_t s_clock_blue = 255;
static uint8_t s_clock_brightness = CLOCK_LED_DEFAULT_BRIGHTNESS;
static char s_clock_mode[16] = "time";
static char s_clock_effect[16] = "normal";
static bool s_clock_dirty = true;
// 0-9数字定义(灯板矩阵)  显示区域:8*5
static const uint8_t s_digit_font[10][5] = {
    {0x07, 0x05, 0x05, 0x05, 0x07}, // 0
    {0x02, 0x06, 0x02, 0x02, 0x07}, // 1
    {0x07, 0x01, 0x07, 0x04, 0x07}, // 2
    {0x07, 0x01, 0x07, 0x01, 0x07}, // 3
    {0x05, 0x05, 0x07, 0x01, 0x01}, // 4
    {0x07, 0x04, 0x07, 0x01, 0x07}, // 5
    {0x07, 0x04, 0x07, 0x05, 0x07}, // 6
    {0x07, 0x01, 0x02, 0x02, 0x02}, // 7
    {0x07, 0x05, 0x07, 0x05, 0x07}, // 8
    {0x07, 0x05, 0x07, 0x01, 0x07}, // 9
};
<<<<<<< HEAD
// WS2812 LED编码器符号定义----1
=======
// WS2812 LED编码器符号定�?---1
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 3,
    .level1 = 0,
    .duration1 = 9,
};
<<<<<<< HEAD
// WS2812 LED编码器符号定义----0
=======
// WS2812 LED编码器符号定�?---0
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 9,
    .level1 = 0,
    .duration1 = 3,
};
<<<<<<< HEAD
// WS2812 LED编码器符号定义----复位
=======
// WS2812 LED编码器符号定�?---复位
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = 500,
    .level1 = 0,
    .duration1 = 500,
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
// 发送W2812b数据
static void ws2812_send_data(rmt_channel_handle_t led_chan,
                             rmt_encoder_handle_t led_encoder,
                             const uint8_t *led_data, size_t led_data_size)
{
  rmt_transmit_config_t tx_config = {
      .loop_count = 0,
  };

  ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_data,
                               led_data_size, &tx_config));
  ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}
/**
<<<<<<< HEAD
 * @brief 根据传入的灯珠X，y坐标值,计算灯珠索引，并将对应的RGB值写入索引中
=======
 * @brief 根据传入的灯珠X，y坐标�?计算灯珠索引，并将对应的RGB值写入索引中
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
 *
 * @param led_data
 * @param x
 * @param y
 * @param red
 * @param green
 * @param blue
 */
static size_t clock_matrix_index(int x, int y)
{
  // Four 8x8 panels are chained horizontally. Each panel is row-major,
  // not serpentine: row end connects to the leftmost LED of the next row.
  int panel = x / CLOCK_LED_PANEL_WIDTH;
  int local_x = x % CLOCK_LED_PANEL_WIDTH;

  return (size_t)panel * CLOCK_LED_PANEL_WIDTH * CLOCK_LED_PANEL_HEIGHT +
         (size_t)y * CLOCK_LED_PANEL_WIDTH + local_x;
}

static void set_matrix_pixel(uint8_t *led_data, int x, int y, uint8_t red,
                             uint8_t green, uint8_t blue)
{
  if (x < 0 || x >= CLOCK_LED_MATRIX_WIDTH ||
      y < 0 || y >= CLOCK_LED_MATRIX_HEIGHT)
  {
    return;
  }

  size_t index = clock_matrix_index(x, y);
  led_data[index * 3] = green;
  led_data[index * 3 + 1] = red;
  led_data[index * 3 + 2] = blue;
}

static uint8_t scale_brightness(uint8_t value, uint8_t brightness);

static void set_matrix_pixel_scaled(uint8_t *led_data, int x, int y,
                                    uint8_t red, uint8_t green, uint8_t blue,
                                    uint8_t brightness)
{
  set_matrix_pixel(led_data, x, y, scale_brightness(red, brightness),
                   scale_brightness(green, brightness),
                   scale_brightness(blue, brightness));
}
/**
<<<<<<< HEAD
 * @brief 根据传入数字及灯珠的X，y坐标值，计算所有灯珠索引并写入对应的RGB值
 *
=======
 * @brief 根据传入数字及灯珠的X，y坐标值，计算所有灯珠索引并写入对应的RGB�? *
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
 * @param led_data
 * @param digit
 * @param x
 * @param y
 * @param red
 * @param green
 * @param blue
 */
static void __attribute__((unused)) draw_digit(uint8_t *led_data, int digit,
                                               int x, int y, uint8_t red,
                                               uint8_t green, uint8_t blue)
{
<<<<<<< HEAD
  // 数字的合法性判断
=======
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
  if (digit < 0 || digit > 9)
  {
    return;
  }
<<<<<<< HEAD
  // 遍历数字的每一行和每一列，计算灯珠索引并设置对应的RGB值,这里设置的显示尺寸是3列5行
  for (int row = 0; row < 5; row++)
  {
    // 从数字索引中取出每一行的值 digit:当前要显示的数字 row:数字的第几行
    uint8_t bits = s_digit_font[digit][row];
    // 取列,第一列(bit2) 第二列(bit1) 第三列(bit0)
=======

  for (int row = 0; row < 5; row++)
  {
    uint8_t bits = s_digit_font[digit][row];
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
    for (int col = 0; col < 3; col++)
    {
      if (bits & (1 << (2 - col)))
      {
<<<<<<< HEAD
        // 调用计算灯珠索引并写入RGB函数，在当前灯板其实坐标的基础上加上数字的偏移量
=======
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
        set_matrix_pixel(led_data, x + col, y + row, red, green, blue);
      }
    }
  }
}
<<<<<<< HEAD

=======
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
static void draw_colon(uint8_t *led_data, int x, int y, uint8_t red,
                       uint8_t green, uint8_t blue)
{
  set_matrix_pixel(led_data, x, y + 1, red, green, blue);
  set_matrix_pixel(led_data, x, y + 3, red, green, blue);
}

static void draw_digit_clean(uint8_t *led_data, int digit, int x, int y,
                             uint8_t red, uint8_t green, uint8_t blue)
{
  if (digit < 0 || digit > 9)
  {
    return;
  }

  for (int row = 0; row < 5; row++)
  {
    uint8_t bits = s_digit_font[digit][row];
    for (int col = 0; col < 3; col++)
    {
      if (bits & (1 << (2 - col)))
      {
        set_matrix_pixel(led_data, x + col, y + row, red, green, blue);
      }
    }
  }
}

static const uint8_t *small_glyph_for_char(char c)
{
  static const uint8_t glyph_blank[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t glyph_minus[5] = {0x00, 0x00, 0x07, 0x00, 0x00};
  static const uint8_t glyph_c[5] = {0x07, 0x04, 0x04, 0x04, 0x07};
  static const uint8_t glyph_percent[5] = {0x05, 0x01, 0x02, 0x04, 0x05};

  if (c >= '0' && c <= '9')
  {
    return s_digit_font[c - '0'];
  }
  if (c >= 'a' && c <= 'z')
  {
    c -= 32;
  }

  switch (c)
  {
  case '-':
    return glyph_minus;
  case 'C':
    return glyph_c;
  case '%':
    return glyph_percent;
  default:
    return glyph_blank;
  }
}

static void draw_small_char(uint8_t *led_data, char c, int x, int y,
                            uint8_t red, uint8_t green, uint8_t blue)
{
  const uint8_t *glyph = small_glyph_for_char(c);
  for (int row = 0; row < 5; row++)
  {
    uint8_t bits = glyph[row];
    for (int col = 0; col < 3; col++)
    {
      if (bits & (1 << (2 - col)))
      {
        set_matrix_pixel(led_data, x + col, y + row, red, green, blue);
      }
    }
  }
}

static void draw_small_text(uint8_t *led_data, const char *text, int x, int y,
                            uint8_t red, uint8_t green, uint8_t blue)
{
  while (*text && x <= CLOCK_LED_MATRIX_WIDTH - 3)
  {
    draw_small_char(led_data, *text++, x, y, red, green, blue);
    x += 4;
  }
}

static void draw_row_bitmap(uint8_t *led_data, const uint8_t *rows, int width,
                            int height, int x, int y, uint8_t red,
                            uint8_t green, uint8_t blue)
{
  for (int row = 0; row < height; row++)
  {
    uint8_t bits = rows[row];
    for (int col = 0; col < width; col++)
    {
      if (bits & (1 << (width - 1 - col)))
      {
        set_matrix_pixel(led_data, x + col, y + row, red, green, blue);
      }
    }
  }
}

static void draw_temperature_icon(uint8_t *led_data, int x, int y,
                                  uint8_t red, uint8_t green, uint8_t blue)
{
  static const uint8_t icon[7] = {
      0x02, 0x05, 0x05, 0x05, 0x05, 0x07, 0x07,
  };
  draw_row_bitmap(led_data, icon, 3, 7, x, y, red, green, blue);
}

static void draw_drop_icon(uint8_t *led_data, int x, int y, uint8_t red,
                           uint8_t green, uint8_t blue)
{
  static const uint8_t icon[7] = {
      0x02, 0x06, 0x0F, 0x0F, 0x0F, 0x0E, 0x06,
  };
  draw_row_bitmap(led_data, icon, 4, 7, x, y, red, green, blue);
}

static void draw_two_digits(uint8_t *led_data, int value, int *x, int y,
                            uint8_t red, uint8_t green, uint8_t blue)
{
  draw_digit_clean(led_data, value / 10, *x, y, red, green, blue);
  *x += 4;
  draw_digit_clean(led_data, value % 10, *x, y, red, green, blue);
  *x += 4;
}

static void build_clock_frame(uint8_t *led_data, int hour, int minute,
                              int second, uint8_t red, uint8_t green,
                              uint8_t blue)
{
  memset(led_data, 0, CLOCK_LED_NUMBERS * 3);

  int content_width = 27;
  int x = (CLOCK_LED_MATRIX_WIDTH - content_width) / 2;
  int y = (CLOCK_LED_MATRIX_HEIGHT - 5) / 2;

  draw_two_digits(led_data, hour, &x, y, red, green, blue);
  draw_colon(led_data, x, y, red, green, blue);
  x += 2;
  draw_two_digits(led_data, minute, &x, y, red, green, blue);
  draw_colon(led_data, x, y, red, green, blue);
  x += 2;
  draw_two_digits(led_data, second, &x, y, red, green, blue);
}

static int round_sensor_value(float value)
{
  return value >= 0.0f ? (int)(value + 0.5f) : (int)(value - 0.5f);
}

static void draw_temperature_icon_styled(uint8_t *led_data, int x, int y,
                                         uint8_t brightness)
{
  draw_temperature_icon(led_data, x, y, scale_brightness(135, brightness),
                        scale_brightness(215, brightness),
                        scale_brightness(255, brightness));
  set_matrix_pixel_scaled(led_data, x + 1, y + 5, 255, 36, 24, brightness);
  set_matrix_pixel_scaled(led_data, x, y + 6, 255, 36, 24, brightness);
  set_matrix_pixel_scaled(led_data, x + 1, y + 6, 255, 36, 24, brightness);
  set_matrix_pixel_scaled(led_data, x + 2, y + 6, 255, 36, 24, brightness);
}

static void draw_drop_icon_styled(uint8_t *led_data, int x, int y,
                                  uint8_t brightness)
{
  draw_drop_icon(led_data, x, y, scale_brightness(0, brightness),
                 scale_brightness(120, brightness),
                 scale_brightness(255, brightness));
  set_matrix_pixel_scaled(led_data, x + 2, y + 2, 135, 215, 255,
                          brightness);
}

static void build_sensor_frame(uint8_t *led_data, const app_data_t *snap,
                               uint8_t brightness)
{
  char temp_text[4] = "--C";
  char hum_text[4] = "--%";
  uint8_t temp_red = scale_brightness(255, brightness);
  uint8_t temp_green = scale_brightness(220, brightness);
  uint8_t temp_blue = scale_brightness(0, brightness);
  uint8_t hum_red = scale_brightness(0, brightness);
  uint8_t hum_green = scale_brightness(220, brightness);
  uint8_t hum_blue = scale_brightness(255, brightness);

  memset(led_data, 0, CLOCK_LED_NUMBERS * 3);
  draw_temperature_icon_styled(led_data, 0, 0, brightness);
  draw_drop_icon_styled(led_data, 16, 0, brightness);

  if (snap && snap->sensor_ok)
  {
    int temp = round_sensor_value(snap->sensor_temp);
    int hum = round_sensor_value(snap->sensor_humidity);

    if (temp >= -9 && temp <= 99)
    {
      snprintf(temp_text, sizeof(temp_text), temp < 0 ? "%dC" : "%2dC",
               temp);
    }
    if (hum < 0)
    {
      hum = 0;
    }
    if (hum > 99)
    {
      hum = 99;
    }
    snprintf(hum_text, sizeof(hum_text), "%2d%%", hum);
  }

  draw_small_text(led_data, temp_text, 4, 2, temp_red, temp_green,
                  temp_blue);
  draw_small_text(led_data, hum_text, 21, 2, hum_red, hum_green, hum_blue);
}

static bool weather_desc_has(const char *desc, const char *a, const char *b,
                             const char *c)
{
  return (a && strstr(desc, a)) || (b && strstr(desc, b)) ||
         (c && strstr(desc, c));
}

static void draw_sun_icon(uint8_t *led_data, int x, int y, uint8_t brightness)
{
  static const uint8_t icon[8] = {
      0x24, 0x18, 0x7E, 0x3C, 0x3C, 0x7E, 0x18, 0x24,
  };
  draw_row_bitmap(led_data, icon, 8, 8, x, y,
                  scale_brightness(255, brightness),
                  scale_brightness(210, brightness),
                  scale_brightness(0, brightness));
}

static void draw_cloud_icon(uint8_t *led_data, int x, int y,
                            uint8_t brightness)
{
  static const uint8_t icon[8] = {
      0x00, 0x18, 0x3C, 0x7E, 0xFF, 0x7E, 0x00, 0x00,
  };
  draw_row_bitmap(led_data, icon, 8, 8, x, y,
                  scale_brightness(155, brightness),
                  scale_brightness(210, brightness),
                  scale_brightness(255, brightness));
}

static void draw_rain_icon(uint8_t *led_data, int x, int y,
                           uint8_t brightness)
{
  static const uint8_t cloud[5] = {
      0x18, 0x3C, 0x7E, 0xFF, 0x7E,
  };
  draw_row_bitmap(led_data, cloud, 8, 5, x, y,
                  scale_brightness(155, brightness),
                  scale_brightness(210, brightness),
                  scale_brightness(255, brightness));
  for (int i = 0; i < 3; i++)
  {
    int px = x + 1 + i * 3;
    set_matrix_pixel_scaled(led_data, px, y + 6, 0, 150, 255, brightness);
    set_matrix_pixel_scaled(led_data, px + 1, y + 7, 0, 150, 255,
                            brightness);
  }
}

static void draw_snow_icon(uint8_t *led_data, int x, int y,
                           uint8_t brightness)
{
  static const uint8_t icon[8] = {
      0x18, 0x18, 0x5A, 0x3C, 0x3C, 0x5A, 0x18, 0x18,
  };
  draw_row_bitmap(led_data, icon, 8, 8, x, y,
                  scale_brightness(160, brightness),
                  scale_brightness(240, brightness),
                  scale_brightness(255, brightness));
}

static void build_weather_frame(uint8_t *led_data, const app_data_t *snap,
                                uint8_t brightness)
{
  char temp_text[4] = "--C";
  const char *desc = snap ? snap->weather_desc : "";

  memset(led_data, 0, CLOCK_LED_NUMBERS * 3);

  if (!snap || !snap->weather_ok)
  {
    draw_cloud_icon(led_data, 0, 0, brightness);
  }
  else if (weather_desc_has(desc, "Rain", "rain", "Shower") ||
           weather_desc_has(desc, "drizzle", "Drizzle", NULL))
  {
    draw_rain_icon(led_data, 0, 0, brightness);
  }
  else if (weather_desc_has(desc, "Snow", "snow", "Sleet"))
  {
    draw_snow_icon(led_data, 0, 0, brightness);
  }
  else if (weather_desc_has(desc, "Sunny", "sun", "Clear"))
  {
    draw_sun_icon(led_data, 0, 0, brightness);
  }
  else
  {
    draw_cloud_icon(led_data, 0, 0, brightness);
  }

  if (snap && snap->weather_ok && snap->weather_temp[0])
  {
    char *end = NULL;
    long temp = strtol(snap->weather_temp, &end, 10);
    if (end != snap->weather_temp && temp >= -9 && temp <= 99)
    {
      snprintf(temp_text, sizeof(temp_text), temp < 0 ? "%ldC" : "%2ldC",
               temp);
    }
  }

  draw_small_text(led_data, temp_text, 14, 2,
                  scale_brightness(255, brightness),
                  scale_brightness(255, brightness),
                  scale_brightness(255, brightness));
}

static void hsv_wheel(uint8_t pos, uint8_t *red, uint8_t *green,
                      uint8_t *blue)
{
  if (pos < 85)
  {
    *red = 255 - pos * 3;
    *green = pos * 3;
    *blue = 0;
  }
  else if (pos < 170)
  {
    pos -= 85;
    *red = 0;
    *green = 255 - pos * 3;
    *blue = pos * 3;
  }
  else
  {
    pos -= 170;
    *red = pos * 3;
    *green = 0;
    *blue = 255 - pos * 3;
  }
}

static void recolor_lit_pixels_rainbow(uint8_t *led_data, uint8_t brightness,
                                       uint8_t phase)
{
  for (int y = 0; y < CLOCK_LED_MATRIX_HEIGHT; y++)
  {
    for (int x = 0; x < CLOCK_LED_MATRIX_WIDTH; x++)
    {
      size_t index = clock_matrix_index(x, y) * 3;
      if (led_data[index] == 0 && led_data[index + 1] == 0 &&
          led_data[index + 2] == 0)
      {
        continue;
      }
      uint8_t red;
      uint8_t green;
      uint8_t blue;
      hsv_wheel((uint8_t)(phase + x * 6 + y * 18), &red, &green, &blue);
      led_data[index] = scale_brightness(green, brightness);
      led_data[index + 1] = scale_brightness(red, brightness);
      led_data[index + 2] = scale_brightness(blue, brightness);
    }
  }
}

static void build_falling_waterfall_frame(uint8_t *led_data,
                                          uint8_t brightness, uint8_t phase)
{
  memset(led_data, 0, CLOCK_LED_NUMBERS * 3);
  static const uint8_t trail[4] = {255, 150, 70, 28};
  const int fall_span = CLOCK_LED_MATRIX_HEIGHT + 4;

  for (int x = 0; x < CLOCK_LED_MATRIX_WIDTH; x++)
  {
    int head = ((int)phase + x * 3) % fall_span;
    uint8_t base_hue = (uint8_t)(phase * 5 + x * 17);

    for (int t = 0; t < 4; t++)
    {
      int y = head - t;
      if (y < 0 || y >= CLOCK_LED_MATRIX_HEIGHT)
      {
        continue;
      }

      uint8_t red;
      uint8_t green;
      uint8_t blue;
      hsv_wheel((uint8_t)(base_hue + t * 8), &red, &green, &blue);
      red = (uint8_t)(((uint16_t)red * trail[t]) / 255);
      green = (uint8_t)(((uint16_t)green * trail[t]) / 255);
      blue = (uint8_t)(((uint16_t)blue * trail[t]) / 255);
      set_matrix_pixel_scaled(led_data, x, y, red, green, blue, brightness);
    }
  }
}

static void build_scan_frame(uint8_t *led_data, size_t index, uint8_t red,
                             uint8_t green, uint8_t blue)
{
  memset(led_data, 0, CLOCK_LED_NUMBERS * 3);
  if (index >= CLOCK_LED_NUMBERS)
  {
    return;
  }

  led_data[index * 3] = green;
  led_data[index * 3 + 1] = red;
  led_data[index * 3 + 2] = blue;
}

static void current_time_parts(int *hour, int *minute, int *second)
{
  time_t now = time(NULL);
  if (now >= 1700000000)
  {
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    *hour = tm_now.tm_hour;
    *minute = tm_now.tm_min;
    *second = tm_now.tm_sec;
    return;
  }

  int uptime = (int)(esp_timer_get_time() / 1000000LL);
  *hour = (uptime / 3600) % 24;
  *minute = (uptime / 60) % 60;
  *second = uptime % 60;
}

static uint8_t scale_brightness(uint8_t value, uint8_t brightness)
{
  return (uint8_t)(((uint16_t)value * brightness) / 255);
}

static uint8_t clamp_rgb_brightness(uint8_t value)
{
  return value > RGB_LED_MAX_BRIGHTNESS ? RGB_LED_MAX_BRIGHTNESS : value;
}

static bool is_sensor_clock_mode(const char *mode)
{
  return strcmp(mode, "env") == 0 ||
         strcmp(mode, "sensor") == 0 ||
         strcmp(mode, "th") == 0 ||
         strcmp(mode, "temp") == 0 ||
         strcmp(mode, "temp_humidity") == 0 ||
         strcmp(mode, "aht30") == 0;
}

static bool is_weather_clock_mode(const char *mode)
{
  return strcmp(mode, "weather") == 0 ||
         strcmp(mode, "forecast") == 0;
}

static bool is_waterfall_clock_mode(const char *mode)
{
  return strcmp(mode, "waterfall") == 0 ||
         strcmp(mode, "rainbow") == 0 ||
         strcmp(mode, "colorful") == 0;
}

static bool is_rainbow_clock_effect(const char *effect)
{
  return strcmp(effect, "rainbow") == 0 ||
         strcmp(effect, "waterfall") == 0 ||
         strcmp(effect, "colorful") == 0;
}

static void build_solid_frame(uint8_t *led_data, size_t led_count, uint8_t red,
                              uint8_t green, uint8_t blue)
{
  for (size_t i = 0; i < led_count; i++)
  {
    led_data[i * 3] = green;
    led_data[i * 3 + 1] = red;
    led_data[i * 3 + 2] = blue;
  }
}

static void init_ws2812_tx(gpio_num_t gpio, rmt_channel_handle_t *led_chan,
                           rmt_encoder_handle_t *led_encoder, bool try_dma)
{
  rmt_tx_channel_config_t tx_chan_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .gpio_num = gpio,
      .mem_block_symbols = try_dma ? 256 : 64,
      .resolution_hz = RMT_LED_RESOLUTION_HZ,
      .trans_queue_depth = 4,
      .flags.with_dma = try_dma,
  };
  esp_err_t ret = rmt_new_tx_channel(&tx_chan_config, led_chan);
  if (ret != ESP_OK && try_dma)
  {
    ESP_LOGW(TAG, "RMT DMA unavailable on GPIO%d, fallback to normal RMT: %s",
             gpio, esp_err_to_name(ret));
    tx_chan_config.mem_block_symbols = 64;
    tx_chan_config.flags.with_dma = false;
    ret = rmt_new_tx_channel(&tx_chan_config, led_chan);
  }
  ESP_ERROR_CHECK(ret);

  rmt_simple_encoder_config_t encoder_config = {
      .callback = ws2812_encoder_callback,
  };
  ESP_ERROR_CHECK(rmt_new_simple_encoder(&encoder_config, led_encoder));
  ESP_ERROR_CHECK(rmt_enable(*led_chan));
}

static bool rgb_led_take_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
  bool dirty;

  taskENTER_CRITICAL(&s_rgb_mux);
  *red = s_rgb_red;
  *green = s_rgb_green;
  *blue = s_rgb_blue;
  dirty = s_rgb_dirty;
  s_rgb_dirty = false;
  taskEXIT_CRITICAL(&s_rgb_mux);

  return dirty;
}

static bool clock_led_take_state(uint8_t *red, uint8_t *green, uint8_t *blue,
                                 uint8_t *brightness, char *mode,
                                 size_t mode_size, char *effect,
                                 size_t effect_size)
{
  bool dirty;

  taskENTER_CRITICAL(&s_clock_mux);
  *red = s_clock_red;
  *green = s_clock_green;
  *blue = s_clock_blue;
  *brightness = s_clock_brightness;
  strlcpy(mode, s_clock_mode, mode_size);
  strlcpy(effect, s_clock_effect, effect_size);
  dirty = s_clock_dirty;
  s_clock_dirty = false;
  taskEXIT_CRITICAL(&s_clock_mux);

  return dirty;
}

void rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
  taskENTER_CRITICAL(&s_rgb_mux);
  s_rgb_red = clamp_rgb_brightness(red);
  s_rgb_green = clamp_rgb_brightness(green);
  s_rgb_blue = clamp_rgb_brightness(blue);
  s_rgb_dirty = true;
  taskEXIT_CRITICAL(&s_rgb_mux);
}

void rgb_led_get_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
  uint8_t r;
  uint8_t g;
  uint8_t b;

  taskENTER_CRITICAL(&s_rgb_mux);
  r = s_rgb_red;
  g = s_rgb_green;
  b = s_rgb_blue;
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

void clock_led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
  taskENTER_CRITICAL(&s_clock_mux);
  s_clock_red = red;
  s_clock_green = green;
  s_clock_blue = blue;
  s_clock_dirty = true;
  taskEXIT_CRITICAL(&s_clock_mux);
}

void clock_led_get_color(uint8_t *red, uint8_t *green, uint8_t *blue)
{
  uint8_t r;
  uint8_t g;
  uint8_t b;

  taskENTER_CRITICAL(&s_clock_mux);
  r = s_clock_red;
  g = s_clock_green;
  b = s_clock_blue;
  taskEXIT_CRITICAL(&s_clock_mux);

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

void clock_led_set_brightness(uint8_t brightness)
{
  if (brightness > CLOCK_LED_MAX_BRIGHTNESS)
  {
    brightness = CLOCK_LED_MAX_BRIGHTNESS;
  }

  taskENTER_CRITICAL(&s_clock_mux);
  s_clock_brightness = brightness;
  s_clock_dirty = true;
  taskEXIT_CRITICAL(&s_clock_mux);
}

uint8_t clock_led_get_brightness(void)
{
  uint8_t brightness;

  taskENTER_CRITICAL(&s_clock_mux);
  brightness = s_clock_brightness;
  taskEXIT_CRITICAL(&s_clock_mux);

  return brightness;
}

void clock_led_set_mode(const char *mode)
{
  if (!mode || mode[0] == '\0')
  {
    return;
  }

  taskENTER_CRITICAL(&s_clock_mux);
  strlcpy(s_clock_mode, mode, sizeof(s_clock_mode));
  s_clock_dirty = true;
  taskEXIT_CRITICAL(&s_clock_mux);
}

void clock_led_set_effect(const char *effect)
{
  if (!effect || effect[0] == '\0')
  {
    return;
  }

  taskENTER_CRITICAL(&s_clock_mux);
  strlcpy(s_clock_effect, effect, sizeof(s_clock_effect));
  s_clock_dirty = true;
  taskEXIT_CRITICAL(&s_clock_mux);
}

void led_task(void *arg)
{
  (void)arg;
  ESP_LOGI(TAG,
           "init RGB GPIO%d count=%d, clock GPIO%d matrix=%dx%d count=%d",
           RGB_LED_GPIO, RGB_LED_NUMBERS, CLOCK_LED_GPIO,
           CLOCK_LED_MATRIX_WIDTH, CLOCK_LED_MATRIX_HEIGHT,
           CLOCK_LED_NUMBERS);

  rmt_channel_handle_t rgb_chan = NULL;
  rmt_encoder_handle_t rgb_encoder = NULL;
<<<<<<< HEAD
  init_ws2812_tx(RGB_LED_GPIO, &rgb_chan, &rgb_encoder, false);
=======
  bool rgb_enabled = RGB_LED_GPIO != GPIO_NUM_NC;
  if (rgb_enabled)
  {
    init_ws2812_tx(RGB_LED_GPIO, &rgb_chan, &rgb_encoder, false);
  }
  else
  {
    ESP_LOGI(TAG, "RGB LED output disabled");
  }
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c

  rmt_channel_handle_t clock_chan = NULL;
  rmt_encoder_handle_t clock_encoder = NULL;
  init_ws2812_tx(CLOCK_LED_GPIO, &clock_chan, &clock_encoder, true);

  int last_second = -1;
  uint8_t rgb_red = 0;
  uint8_t rgb_green = 0;
  uint8_t rgb_blue = 0;
  uint8_t clock_red = 0;
  uint8_t clock_green = 0;
  uint8_t clock_blue = 0;
  uint8_t clock_brightness = 0;
  char clock_mode[16] = "time";
  char clock_effect[16] = "normal";
  int64_t last_scan_slot = -1;
  size_t scan_index = 0;
  uint32_t last_sensor_seq = UINT32_MAX;
  uint32_t last_weather_seq = UINT32_MAX;
  int64_t last_waterfall_slot = -1;
  uint8_t rgb_data[RGB_LED_NUMBERS * 3];
  uint8_t clock_data[CLOCK_LED_NUMBERS * 3];

  while (1)
  {
    bool rgb_changed = rgb_led_take_color(&rgb_red, &rgb_green, &rgb_blue);
    bool clock_changed = clock_led_take_state(&clock_red, &clock_green,
                                              &clock_blue, &clock_brightness,
                                              clock_mode, sizeof(clock_mode),
                                              clock_effect,
                                              sizeof(clock_effect));
    int hour;
    int minute;
    int second;
    current_time_parts(&hour, &minute, &second);

<<<<<<< HEAD
    if (rgb_changed)
=======
    if (rgb_changed && rgb_enabled)
>>>>>>> 8787c19c9d188db3f5512c973ff257d8d540595c
    {
      build_solid_frame(rgb_data, RGB_LED_NUMBERS, rgb_red, rgb_green,
                        rgb_blue);
      ws2812_send_data(rgb_chan, rgb_encoder, rgb_data, sizeof(rgb_data));
      ESP_LOGI(TAG, "GPIO%d RGB set to r=%u g=%u b=%u", RGB_LED_GPIO,
               rgb_red, rgb_green, rgb_blue);
    }

    bool scan_mode = strcmp(clock_mode, "test") == 0 ||
                     strcmp(clock_mode, "scan") == 0;
    bool sensor_mode = is_sensor_clock_mode(clock_mode);
    bool weather_mode = is_weather_clock_mode(clock_mode);
    bool waterfall_mode = is_waterfall_clock_mode(clock_mode);
    bool rainbow_effect = is_rainbow_clock_effect(clock_effect);
    int64_t scan_slot = esp_timer_get_time() / 100000LL;
    int64_t waterfall_slot = esp_timer_get_time() / 90000LL;

    if (scan_mode && (clock_changed || scan_slot != last_scan_slot))
    {
      uint8_t scaled_red = scale_brightness(clock_red, clock_brightness);
      uint8_t scaled_green = scale_brightness(clock_green, clock_brightness);
      uint8_t scaled_blue = scale_brightness(clock_blue, clock_brightness);
      build_scan_frame(clock_data, scan_index, scaled_red, scaled_green,
                       scaled_blue);
      ws2812_send_data(clock_chan, clock_encoder, clock_data,
                       sizeof(clock_data));
      scan_index = (scan_index + 1) % CLOCK_LED_NUMBERS;
      last_scan_slot = scan_slot;
    }
    else if (waterfall_mode)
    {
      if (clock_changed || waterfall_slot != last_waterfall_slot)
      {
        build_falling_waterfall_frame(clock_data, clock_brightness,
                                      (uint8_t)waterfall_slot);
        ws2812_send_data(clock_chan, clock_encoder, clock_data,
                         sizeof(clock_data));
        last_waterfall_slot = waterfall_slot;
      }
    }
    else if (sensor_mode)
    {
      app_data_t snap;
      app_data_snapshot(&snap);
      if (clock_changed || snap.sensor_seq != last_sensor_seq)
      {
        build_sensor_frame(clock_data, &snap, clock_brightness);
        if (rainbow_effect)
        {
          recolor_lit_pixels_rainbow(clock_data, clock_brightness,
                                     (uint8_t)(waterfall_slot * 7));
        }
        ws2812_send_data(clock_chan, clock_encoder, clock_data,
                         sizeof(clock_data));
        last_sensor_seq = snap.sensor_seq;
        if (clock_changed)
        {
          ESP_LOGI(TAG,
                   "GPIO%d sensor clock mode color=%u,%u,%u brightness=%u",
                   CLOCK_LED_GPIO, clock_red, clock_green, clock_blue,
                   clock_brightness);
        }
      }
    }
    else if (weather_mode)
    {
      app_data_t snap;
      app_data_snapshot(&snap);
      if (clock_changed || snap.weather_seq != last_weather_seq)
      {
        build_weather_frame(clock_data, &snap, clock_brightness);
        if (rainbow_effect)
        {
          recolor_lit_pixels_rainbow(clock_data, clock_brightness,
                                     (uint8_t)(waterfall_slot * 7));
        }
        ws2812_send_data(clock_chan, clock_encoder, clock_data,
                         sizeof(clock_data));
        last_weather_seq = snap.weather_seq;
        if (clock_changed)
        {
          ESP_LOGI(TAG,
                   "GPIO%d weather clock mode effect=%s brightness=%u",
                   CLOCK_LED_GPIO, clock_effect, clock_brightness);
        }
      }
    }
    else if (clock_changed || second != last_second)
    {
      uint8_t scaled_red = scale_brightness(clock_red, clock_brightness);
      uint8_t scaled_green = scale_brightness(clock_green, clock_brightness);
      uint8_t scaled_blue = scale_brightness(clock_blue, clock_brightness);
      build_clock_frame(clock_data, hour, minute, second, scaled_red,
                        scaled_green, scaled_blue);
      if (rainbow_effect)
      {
        recolor_lit_pixels_rainbow(clock_data, clock_brightness,
                                   (uint8_t)(waterfall_slot * 7));
      }
      ws2812_send_data(clock_chan, clock_encoder, clock_data,
                       sizeof(clock_data));
      last_second = second;
      if (clock_changed)
      {
        ESP_LOGI(TAG,
                 "GPIO%d clock set to r=%u g=%u b=%u brightness=%u",
                 CLOCK_LED_GPIO, clock_red, clock_green, clock_blue,
                 clock_brightness);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
