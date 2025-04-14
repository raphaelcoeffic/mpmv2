#pragma once

#include <stdint.h>
#include "spi.h"

// GRB pixel format (from lsb to msb)
#define LED_COLOR(r, g, b) (((b) << 16) | ((r) << 8) | (g))

void led_rgb_init(spi_t spi, uint32_t dio);
void led_rgb_set_color(uint32_t color);
