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

// FASST RX (sniffer) on hop k: each packet despread, printed as
// @fasst <time_us> <k> rssi=<dBm> [foff=<kHz>] shift=<n> <hex> q=<hex>: the
// hop it came on, the packet's first bytes (bits LSB first, as on air) and
// per byte its worst bit correlation (0..b, b = all 11 chips right). shift:
// bits from the RF core's sync word to the real one; sync=none instead if
// not found. With follow, a 10ch packet with a good CRC locks on the hops
// (fasst_unpack(): the next hop and its time); an unknown format stays
// parked on k. FASSTest (CRC unchecked, the whitening needs the TX ID): a
// TX packet with clean chips and a known hop code locks, and the RX stays
// on its channel until just before the next hop, for the receiver's reply
//
// The RF core also finds its sync word at the very start of a packet (the
// sender's synthesizer settling; the sync bits 6..15 came 68 bits later):
// the RX takes the whole preamble too
#define FASST_RX_EARLY_BITS 96
// FASST_RX_LEN() within RADIO_MAX_PKT_LEN, 2 bytes spare for the despreader
// (the 11-chip limits, the 7-chip mode taking the same)
#define FASST_RX_BITS_MAX ((8 * (RADIO_MAX_PKT_LEN - 2) - FASST_RX_SKIP_CHIPS(11)) / 11)
#define FASST_RX_BYTES_MAX ((FASST_RX_BITS_MAX - FASST_RX_SYNC_LEFT(11) - FASST_RX_EARLY_BITS) / 8)
typedef struct {
  uint32_t secs;           // 0: until stopped
  uint8_t k;               // hop index 0..35
  uint16_t bytes;          // packet bytes to print, 1..FASST_RX_BYTES_MAX
  uint8_t rx_bw;           // RF core RX bandwidth code
  uint8_t sync_threshold;  // 0 = default
  bool follow;             // follow the hops (10ch, FASSTest)
  bool fasstest;           // FASSTest: k = its channel 0..75 (FASSTEST_CHANNELS)
  uint8_t chips;           // chips per bit: FASST_BARKER_11, or _7 (FASSTest 12CH)
  bool (*stop)();          // true: end the run now (the host asked), or NULL
} fasst_rx_cfg_t;

// Blocks for cfg->secs or until cfg->stop(); the radio needs radio_init()
// again after it
void fasst_rx_run(const fasst_rx_cfg_t* cfg, fasst_print_t print);
