#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#define RGB_LED_GPIO GPIO_NUM_48
#define RGB_LED_NUMBERS 1
#define RMT_LED_RESOLUTION_HZ 10000000

#define PORTAL_AP_SSID "ESP32"
#define PORTAL_AP_PASS "12345678"
#define PORTAL_AP_CHANNEL 1
#define PORTAL_MAX_CONN 4

#define ROUTER_WIFI_SSID "ZTE-516"
#define ROUTER_WIFI_PASS "15272721445"

#define WEATHER_LOCATION "Suzhou"
#define WEATHER_HOST "wttr.in"

#define MQTT_BROKER_URI "mqtt://broker.emqx.io:1883"
#define MQTT_TOPIC_PREFIX "iot/v1/devices"
#define MQTT_PUBLISH_INTERVAL_MS 1000

#define AHT30_I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO GPIO_NUM_1
#define I2C_SCL_GPIO GPIO_NUM_2
#define AHT30_ADDR 0x38
#define AHT30_I2C_FREQ_HZ 100000
#define I2C_TIMEOUT_MS 1000

#define OLED_I2C_ADDR 0x3C
#define OLED_I2C_FREQ_HZ 400000
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_RST_GPIO GPIO_NUM_NC

#define DNS_PORT 53
#define DNS_MAX_LEN 256
#define DHCPS_OFFER_DNS 0x02
