#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

// SX1281 in GFSK packet mode, as a raw packet radio like radio.h: preamble +
// sync word (1 to 4 bytes) + fixed length data, no CRC and no whitening (the
// framing is left to frame.c). See doc/SX1280-81_Rev3.2_Mar2020.pdf.
//
// It shares the PA/LNA with the CC2652 through U3: sx1281_init() switches the
// front-end to the SX1281 and drives CTX itself, sx1281_off() gives it back
// to the CC2652 RF core (RFC_GPO1).

// The SKY66112 input is +5 dBm max, -2 dBm recommended (board.h)
#define SX1281_TX_DBM_DEFAULT 0
#define SX1281_TX_DBM_MAX 2

// GFSK settings picked for a PHY: bit rates 125 kbps to 2 Mbps only
typedef struct {
  uint8_t br_bw;    // modParam1
  uint8_t mod_ind;  // modParam2
  uint8_t bt;       // modParam3
  uint32_t rate;    // bps
  uint32_t bw;      // DSB bandwidth, Hz
  uint32_t dev;     // deviation actually sent, Hz
} sx1281_mod_t;

// Resets the SX1281 and sets it up for phy (rate, deviation, preamble, sync
// word size, 2-FSK or GFSK BT 0.5). tx_dbm: -18 to SX1281_TX_DBM_MAX
radio_status_t sx1281_init(const radio_phy_t* phy, int8_t tx_dbm);

// Holds the SX1281 in reset, front-end back to the CC2652
void sx1281_off();

bool sx1281_active();

// Settings chosen by sx1281_init()
const sx1281_mod_t* sx1281_mod();

radio_status_t sx1281_set_frequency(uint32_t freq_khz);

// Sync words for TX and RX, right aligned on the sync_bits of sx1281_init()
void sx1281_set_sync_word(uint32_t tx_sync, uint32_t rx_sync);

// Sync word bit errors tolerated in RX (0 to 15)
void sx1281_set_sync_errors(uint8_t errors);

// Send one packet (up to 255 bytes), blocks until sent
radio_status_t sx1281_send(const uint8_t* data, uint16_t len);

// Receive packets of len bytes until sx1281_rx_stop(): RX single mode,
// restarted after each packet by sx1281_receive() (continuous mode can lock
// up under heavy traffic, errata 16.1)
radio_status_t sx1281_rx_start(uint16_t len);
void sx1281_rx_stop();
bool sx1281_receive(radio_packet_t* pkt);

// Receive counters since sx1281_rx_start(): packets, RX armed again after
// the chip left it without a packet, BUSY time-outs, and the status byte and
// IRQ flags seen at the last re-arm
typedef struct {
  uint32_t ok;
  uint32_t rearm;
  uint32_t busy;
  uint8_t last_status;
  uint16_t last_irq;
} sx1281_stats_t;

void sx1281_rx_stats(sx1281_stats_t* stats);

// Last command status byte, for diagnostics
uint16_t sx1281_last_status();
