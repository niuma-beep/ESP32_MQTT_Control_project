#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void format_uptime_clock(char *dst, size_t dst_size, uint64_t now_us);
bool format_local_time(char *dst, size_t dst_size);
void start_sntp(void);
