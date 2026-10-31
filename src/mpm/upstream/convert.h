#pragma once

// FrSkyDVX_common.ino's channel scaling in Convert.ino (convert.c): num_chan
// and its default 8 are C++ default arguments
#include "mpm_compat.h"

uint16_t FrSkyX_scaleForPXX_n(uint8_t i, uint8_t num_chan);
uint16_t FrSkyX_scaleForPXX_FS_n(uint8_t i, uint8_t num_chan);
#define FrSkyX_scaleForPXX(...) _FRSKY_DEF8(FrSkyX_scaleForPXX_n, __VA_ARGS__, 8, 0)
#define FrSkyX_scaleForPXX_FS(...) _FRSKY_DEF8(FrSkyX_scaleForPXX_FS_n, __VA_ARGS__, 8, 0)
#define _FRSKY_DEF8(f, a, b, ...) f(a, b)
