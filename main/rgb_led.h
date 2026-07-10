#pragma once

#include <stdint.h>

void led_task(void *arg);
void rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue);
void rgb_led_get_color(uint8_t *red, uint8_t *green, uint8_t *blue);
void clock_led_set_color(uint8_t red, uint8_t green, uint8_t blue);
void clock_led_get_color(uint8_t *red, uint8_t *green, uint8_t *blue);
void clock_led_set_brightness(uint8_t brightness);
uint8_t clock_led_get_brightness(void);
void clock_led_set_mode(const char *mode);
void clock_led_set_effect(const char *effect);
