#include "oled_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_state.h"
#include "app_time.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "oled";

static const uint8_t font5x7[][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['%'] = {0x62, 0x64, 0x08, 0x13, 0x23},
    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
    [':'] = {0x00, 0x36, 0x36, 0x00, 0x00},
    ['/'] = {0x20, 0x10, 0x08, 0x04, 0x02},
    ['0'] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1'] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3'] = {0x21, 0x41, 0x45, 0x4B, 0x31},
    ['4'] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6'] = {0x3C, 0x4A, 0x49, 0x49, 0x30},
    ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1E},
    ['A'] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ['B'] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D'] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E'] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F'] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G'] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
    ['H'] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I'] = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ['J'] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K'] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L'] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M'] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N'] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O'] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P'] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['Q'] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['R'] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T'] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U'] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V'] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['W'] = {0x7F, 0x20, 0x18, 0x20, 0x7F},
    ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07},
    ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
    ['_'] = {0x40, 0x40, 0x40, 0x40, 0x40},
};

static void oled_draw_char(uint8_t *buf, int x, int y, char c)
{
  if (c >= 'a' && c <= 'z')
  {
    c -= 32;
  }
  uint8_t glyph_index = (uint8_t)c;
  const uint8_t *glyph = glyph_index < (sizeof(font5x7) / 5) ? font5x7[glyph_index] : font5x7[(int)' '];

  for (int col = 0; col < 5; col++)
  {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 7; row++)
    {
      if (bits & (1 << row))
      {
        int px = x + col;
        int py = y + row;
        if (px >= 0 && px < OLED_WIDTH && py >= 0 && py < OLED_HEIGHT)
        {
          buf[(py / 8) * OLED_WIDTH + px] |= (1 << (py % 8));
        }
      }
    }
  }
}

static void oled_set_pixel(uint8_t *buf, int x, int y)
{
  if (x >= 0 && x < OLED_WIDTH && y >= 0 && y < OLED_HEIGHT)
  {
    buf[(y / 8) * OLED_WIDTH + x] |= (1 << (y % 8));
  }
}

static void oled_draw_hline(uint8_t *buf, int x, int y, int w)
{
  for (int i = 0; i < w; i++)
  {
    oled_set_pixel(buf, x + i, y);
  }
}

static void oled_draw_vline(uint8_t *buf, int x, int y, int h)
{
  for (int i = 0; i < h; i++)
  {
    oled_set_pixel(buf, x, y + i);
  }
}

static void oled_draw_rect(uint8_t *buf, int x, int y, int w, int h)
{
  oled_draw_hline(buf, x, y, w);
  oled_draw_hline(buf, x, y + h - 1, w);
  oled_draw_vline(buf, x, y, h);
  oled_draw_vline(buf, x + w - 1, y, h);
}

static void oled_draw_text(uint8_t *buf, int x, int y, const char *text)
{
  while (*text && x <= OLED_WIDTH - 6)
  {
    oled_draw_char(buf, x, y, *text++);
    x += 6;
  }
}

static void oled_draw_wifi_icon(uint8_t *buf, int x, int y, bool connected,
                                int anim)
{
  oled_set_pixel(buf, x + 6, y + 10);
  if (!connected)
  {
    oled_draw_hline(buf, x + 1, y + 1, 10);
    oled_draw_hline(buf, x + 2, y + 2, 8);
    oled_draw_hline(buf, x + 3, y + 3, 6);
    oled_draw_hline(buf, x + 4, y + 4, 4);
    oled_draw_vline(buf, x + 1, y + 8, 4);
    oled_draw_vline(buf, x + 10, y + 8, 4);
    return;
  }

  int bars = 2 + (anim % 3);
  for (int b = 0; b < bars; b++)
  {
    int px = x + 3 + b * 3;
    int h = 3 + b * 2;
    oled_draw_vline(buf, px, y + 10 - h, h);
    oled_draw_vline(buf, px + 1, y + 10 - h, h);
  }
}

static void oled_draw_weather_icon(uint8_t *buf, int x, int y,
                                   const char *desc, bool ok, int anim)
{
  if (!ok)
  {
    oled_draw_rect(buf, x + 1, y + 1, 10, 10);
    oled_draw_hline(buf, x + 3, y + 5, 6);
    return;
  }

  const char *d = desc ? desc : "";
  if (strstr(d, "Rain") || strstr(d, "rain") || strstr(d, "Shower") ||
      strstr(d, "drizzle"))
  {
    oled_draw_hline(buf, x + 2, y + 2, 7);
    oled_draw_hline(buf, x + 1, y + 3, 10);
    oled_draw_hline(buf, x + 3, y + 4, 8);
    for (int i = 0; i < 4; i++)
    {
      int px = x + 2 + i * 3;
      int py = y + 7 + ((anim + i) % 2);
      oled_set_pixel(buf, px, py);
      oled_set_pixel(buf, px, py + 1);
    }
  }
  else if (strstr(d, "Snow") || strstr(d, "snow") || strstr(d, "Sleet"))
  {
    oled_draw_hline(buf, x + 2, y + 5, 9);
    oled_draw_vline(buf, x + 6, y + 1, 9);
    oled_set_pixel(buf, x + 3, y + 2);
    oled_set_pixel(buf, x + 9, y + 2);
    oled_set_pixel(buf, x + 3, y + 8);
    oled_set_pixel(buf, x + 9, y + 8);
  }
  else if (strstr(d, "Sunny") || strstr(d, "sun") || strstr(d, "Clear"))
  {
    oled_draw_rect(buf, x + 4, y + 4, 5, 5);
    if (anim % 2)
    {
      oled_set_pixel(buf, x + 6, y);
      oled_set_pixel(buf, x + 6, y + 12);
      oled_set_pixel(buf, x, y + 6);
      oled_set_pixel(buf, x + 12, y + 6);
    }
  }
  else
  {
    oled_draw_hline(buf, x + 2, y + 4, 7);
    oled_draw_hline(buf, x + 1, y + 5, 10);
    oled_draw_hline(buf, x + 3, y + 6, 8);
    oled_draw_hline(buf, x + 2 + (anim % 2), y + 8, 8);
  }
}

void oled_task(void *arg)
{
  (void)arg;

  ESP_LOGI(TAG, "init OLED SSD1306 addr=0x%02X", OLED_I2C_ADDR);

  esp_lcd_panel_io_handle_t io_handle = NULL;
  esp_lcd_panel_io_i2c_config_t io_config = {
      .dev_addr = OLED_I2C_ADDR,
      .scl_speed_hz = OLED_I2C_FREQ_HZ,
      .control_phase_bytes = 1,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .dc_bit_offset = 6,
  };
  esp_err_t ret = esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &io_handle);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "OLED panel IO init failed: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
  }

  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_ssd1306_config_t ssd1306_config = {
      .height = OLED_HEIGHT,
  };
  esp_lcd_panel_dev_config_t panel_config = {
      .bits_per_pixel = 1,
      .reset_gpio_num = OLED_RST_GPIO,
      .vendor_config = &ssd1306_config,
  };
  ret = esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "OLED SSD1306 driver init failed: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
  }
  ret = esp_lcd_panel_reset(panel_handle);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "OLED reset failed: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
  }
  ret = esp_lcd_panel_init(panel_handle);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
  }
  ret = esp_lcd_panel_disp_on_off(panel_handle, true);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "OLED display on failed: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
  }

  uint8_t frame[OLED_WIDTH * OLED_HEIGHT / 8];
  char line[32];
  int anim = 0;

  while (1)
  {
    app_data_t snap;
    uint64_t now_us = esp_timer_get_time();
    app_data_snapshot(&snap);

    memset(frame, 0, sizeof(frame));
    if (format_local_time(line, sizeof(line)))
    {
      oled_draw_text(frame, 0, 0, line);
    }
    else
    {
      format_uptime_clock(line, sizeof(line), now_us);
      oled_draw_text(frame, 0, 0, "UP");
      oled_draw_text(frame, 18, 0, line);
    }
    oled_draw_wifi_icon(frame, 84, 0, snap.sta_connected, anim);
    oled_draw_weather_icon(frame, 104, 0, snap.weather_desc, snap.weather_ok,
                           anim);
    if (anim % 2)
    {
      oled_set_pixel(frame, 126, 1);
      oled_set_pixel(frame, 126, 3);
    }
    oled_draw_hline(frame, 0, 13, OLED_WIDTH);

    oled_draw_text(frame, 0, 16, "IN");
    oled_draw_text(frame, 64, 16, "OUT");

    if (snap.sensor_ok)
    {
      snprintf(line, sizeof(line), "T:%4.1fC", snap.sensor_temp);
      oled_draw_text(frame, 0, 27, line);
      snprintf(line, sizeof(line), "H:%4.1f%%", snap.sensor_humidity);
      oled_draw_text(frame, 0, 38, line);
    }
    else
    {
      oled_draw_text(frame, 0, 27, "T:--.-C");
      oled_draw_text(frame, 0, 38, "H:--.-%");
    }

    if (snap.weather_ok)
    {
      snprintf(line, sizeof(line), "T:%sC", snap.weather_temp);
      oled_draw_text(frame, 64, 27, line);
      snprintf(line, sizeof(line), "H:%s%%", snap.weather_humidity);
      oled_draw_text(frame, 64, 38, line);
    }
    else
    {
      oled_draw_text(frame, 64, 27, "T:--C");
      oled_draw_text(frame, 64, 38, "H:--%");
    }

    oled_draw_hline(frame, 0, 50, OLED_WIDTH);
    oled_draw_text(frame, 0, 54, snap.sta_connected ? "NET:OK" : "NET:AP");
    oled_draw_text(frame, 50, 54, snap.weather_ok ? "W:OK" : "W:--");
    if (snap.weather_ok)
    {
      if (strstr(snap.weather_desc, "Rain") || strstr(snap.weather_desc, "rain"))
      {
        oled_draw_text(frame, 88, 54, "RAIN");
      }
      else if (strstr(snap.weather_desc, "Sunny") ||
               strstr(snap.weather_desc, "sun") ||
               strstr(snap.weather_desc, "Clear"))
      {
        oled_draw_text(frame, 88, 54, "SUN");
      }
      else
      {
        oled_draw_text(frame, 88, 54, "CLOUD");
      }
    }

    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, OLED_WIDTH, OLED_HEIGHT,
                              frame);
    anim++;
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
