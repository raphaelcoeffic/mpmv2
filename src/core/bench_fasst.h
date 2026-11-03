#pragma once

#include <stdbool.h>
#include <stdint.h>

// FASST 10ch TX experiment (fasst_frame.h): packets with a chosen ID, hop
// step, chip rate and deviation, for the receiver's margins and binding
typedef struct {
  uint32_t secs;
  uint32_t chip_rate;  // chips/s
  uint32_t deviation;  // Hz
  uint16_t tx_power;   // RF core TX power word
  bool invert;         // chip polarity: Barker = the lower frequency
  uint32_t fs_every;   // frames between F/S cycles (mode bit 5), 0 = none
  uint16_t sweep;      // ch1 sweep, frames per step of 16, 0 = fixed
  uint8_t step;        // hop step (1..36)
  uint32_t id;         // plain ID, bytes 0..3 (fasst_set_id())
} fasst_cfg_t;

typedef void (*fasst_print_t)(const char* fmt, ...);

// Blocks for cfg->secs; the radio needs radio_init() again after it
void fasst_run(const fasst_cfg_t* cfg, fasst_print_t print);
