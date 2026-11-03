#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fasst_frame.h"

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

// FASST RX (sniffer), parked on hop k: each packet despread, printed as
// @fasst <time_us> <k> rssi=<dBm> [foff=<kHz>] shift=<n> <hex> q=<hex>: the
// packet's first bytes (bits LSB first, as on air) and per byte its worst
// bit correlation (0..b, b = all 11 chips right). shift: bits from the RF
// core's sync word to the real one; sync=none instead if not found
//
// The RF core also finds its sync word at the very start of a packet (the
// sender's synthesizer settling; the sync bits 6..15 came 68 bits later):
// the RX takes the whole preamble too
#define FASST_RX_EARLY_BITS 96
// FASST_RX_LEN() within RADIO_MAX_PKT_LEN, 2 bytes spare for the despreader
#define FASST_RX_BITS_MAX ((8 * (RADIO_MAX_PKT_LEN - 2) - FASST_RX_SKIP_CHIPS) / 11)
#define FASST_RX_BYTES_MAX ((FASST_RX_BITS_MAX - FASST_RX_SYNC_LEFT - FASST_RX_EARLY_BITS) / 8)
typedef struct {
  uint32_t secs;
  uint8_t k;               // hop index 0..35
  uint16_t bytes;          // packet bytes to print, 1..FASST_RX_BYTES_MAX
  uint8_t rx_bw;           // RF core RX bandwidth code
  uint8_t sync_threshold;  // 0 = default
} fasst_rx_cfg_t;

// Blocks for cfg->secs; the radio needs radio_init() again after it
void fasst_rx_run(const fasst_rx_cfg_t* cfg, fasst_print_t print);
