#pragma once
#include <stdint.h>
static inline uint32_t esp_random(void) { static uint32_t n=1; return n++; }
