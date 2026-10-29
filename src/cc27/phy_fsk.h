// GFSK at any symbol rate and deviation: TI's 250 kbps PHY (phy_fsk250,
// genfsk images; above 1.2 Mbps NESB 2 Mbps, ble5 images), its rate,
// deviation and the demodulator's decimation changed after the setup.
//
// The demodulator runs at 4 samples per symbol whatever the rate: after the
// decimators (BDE1, BDE2) a fractional resampler (DEMFRAC P/Q, which LRF
// computes from BAUD and DEMMISC3 at each frequency change) takes the samples
// down to 4 x rate x PDIF decimation, PDIF then by its decimation to 4 per
// symbol. The channel bandwidth follows the samples' rate before the
// resampler, so a wider signal (deviation) takes a larger PDIF decimation.

#pragma once

#include <stdint.h>

typedef struct {
  uint32_t rate;       // symbol rate (Hz)
  uint32_t deviation;  // TX deviation (Hz); the RX bandwidth follows it
  int8_t log2_pdif;    // PDIF decimation, log2 (0 to 3); -1: from the deviation
} phy_fsk_t;

// What phy_fsk_setup() programmed
typedef struct {
  uint32_t rate;  // the rate word's rate (Hz)
  uint8_t prescaler;
  uint32_t rate_word;  // 21 bits
  uint8_t bde1, bde2, pdif;
  uint16_t demmisc3;
} phy_fsk_info_t;

// rf_setup() of the base PHY, then the changes. Returns rf_setup()'s result
int phy_fsk_setup(const phy_fsk_t *phy, phy_fsk_info_t *info);
