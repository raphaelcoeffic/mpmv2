#pragma once

#include <stdint.h>

// RF output: antenna 1 (J2) or 2 (J3) on a board with two (ANT_SEL), the
// only one otherwise. Switched in 400 ns (SKY66112), between packets
void board_set_antenna(uint8_t antenna);
uint8_t board_antenna();
