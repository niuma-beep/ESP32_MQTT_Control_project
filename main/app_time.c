#include "app_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "time";
static bool s_sntp_started;

void format_uptime_clock(char *dst, size_t dst_size, uint64_t now_us)
{
  uint32_t total = (uint32_t)(now_us / 1000000ULL);
  uint32_t h = (total / 3600) % 24;
  uint32_t m = (total / 60) % 60;
  uint32_t s = total % 60;
  snprintf(dst, dst_size, "%02lu:%02lu:%02lu", (unsigned long)h,
           (unsigned long)m, (unsigned long)s);
}

bool format_local_time(char *dst, size_t dst_size)
{
  time_t now = time(NULL);
  if (now < 1700000000)
  {
    return false;
  }

  struct tm tm_now;
  localtime_r(&now, &tm_now);
  strftime(dst, dst_size, "%H:%M:%S", &tm_now);
  return true;
}

static void time_sync_cb(struct timeval *tv)
{
  (void)tv;
  ESP_LOGI(TAG, "SNTP time synchronized");
}

void start_sntp(void)
{
  if (s_sntp_started)
  {
    return;
  }

  setenv("TZ", "CST-8", 1);
  tzset();
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_set_time_sync_notification_cb(time_sync_cb);
  esp_sntp_setservername(0, "ntp.aliyun.com");
  esp_sntp_setservername(1, "time.windows.com");
  esp_sntp_init();
  s_sntp_started = true;
}
