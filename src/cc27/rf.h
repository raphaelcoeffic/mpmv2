// CC27xx radio: TI's LRF layer for the setup (PHY, trims, synthesizer, TX
// power), the PBE's generic TX and RX operations driven directly, one at a
// time, polled (no interrupts). Raw framing: preamble, a sync word of up to
// 32 bits, a fixed length payload; no header, length field or CRC (done in
// software, as the CC2500 and NRF24 emulations do).
//
// Packets longer than RF_FIFO_PAYLOAD (CYRF6936 SDR: 1 KB on air) stream
// through the FIFOs, while the CPU polls (rf_busy(), rf_rx_read()): TX
// refilled as the PBE sends, RX copied out as it arrives. At 1 Mbps a FIFO
// lasts 2 ms (RX) or 4 ms (TX): no other work in between.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <ti/drivers/rcl/LRF.h>

// The FIFOs of TI's PHYs: RX 268 bytes for an entry (payload and 12 bytes
// of status), TX 536. Longer packets: streamed
#define RF_FIFO_PAYLOAD 255
#define RF_MAX_PAYLOAD 1100

// Operation results: the PBE's end cause, or one of these
#define RF_OK 0
#define RF_ERR_SETUP -1     // LRF_setupRadio() failed
#define RF_ERR_TIMEOUT -2   // no OPDONE / OPERROR in time
#define RF_ERR_NO_PACKET -3 // RX: nothing in the window
#define RF_ERR_PARAM -4     // length

#define RF_RSSI_NONE (-128)

void rf_init(void);

// PHY setup (a generated LRF_Config, see tools/cc27_phy.py), then the raw
// framing on top of it. Returns LRF_SetupResult (0: ok)
int rf_setup(const LRF_Config *config);

// Modem registers that differ between TX and RX (phy_fsk): DEMMISC3 (the
// PDIF decimation slows the modulator too), BAUD and BAUDPRE (RX at a
// multiple of the rate). Written before LRF_programFrequency(), which
// computes the resampler from them. 0: the PHY's. rf_setup() clears them
typedef struct {
  uint16_t demmisc3, baud, baudpre;
} rf_modem_t;
void rf_set_modem(const rf_modem_t *tx, const rf_modem_t *rx);

// Sync word, MSB first as on air, bits (8 to 32) right-aligned
void rf_set_sync(uint32_t sync, uint8_t bits);

// Sync words A (sent, searched) and B (searched too)
void rf_set_syncs(uint32_t sync_a, uint32_t sync_b, uint8_t bits);

// TX power in dBm (from the PA table: the next level up)
int rf_set_power(int8_t dbm);

// SYSTIM time (0.25 us), the operations' start times
uint32_t rf_time(void);

typedef struct {
  uint16_t len;
  int8_t rssi;
  // Frequency estimate: the sender above us, in about deviation / 72 steps
  // (measured from 31 to 250 kbps, compressed beyond 60 steps)
  int8_t freq_est;
  uint32_t time;  // SYSTIM at sync (0.25 us)
  uint8_t data[RF_MAX_PAYLOAD];
} rf_packet_t;

// Blocking: one packet (rf_tx), one packet of len bytes (2 at least)
// within timeout_us (rf_rx)
int rf_tx(uint32_t freq_hz, const uint8_t *data, uint16_t len);
int rf_rx(uint32_t freq_hz, uint16_t len, uint32_t timeout_us, rf_packet_t *pkt);

// Non-blocking: the operation starts at SYSTIM time at (0: as soon as
// possible, about 100 us), a running one stopped first. RX: packets of len
// bytes, one (repeat false) or until rf_stop(). TX: data stays valid until
// the end (a long packet is sent from it)
int rf_tx_start(uint32_t freq_hz, const uint8_t *data, uint16_t len, uint32_t at);
int rf_rx_start(uint32_t freq_hz, uint16_t len, bool repeat, uint32_t at);

// True while the operation runs. Streams long packets: call it often
bool rf_busy(void);

// A packet received (true), from the RX FIFO
bool rf_rx_read(rf_packet_t *pkt);

// Ends the operation (hard stop if it still runs: RF_ERR_TIMEOUT), the radio
// off. Returns its end cause
int rf_stop(void);

// Instant RSSI (dBm) while an operation runs, RF_RSSI_NONE if not valid
int8_t rf_rssi(void);

// Last end cause and events, for debugging
uint16_t rf_last_endcause(void);
uint32_t rf_last_events(void);

// The PBE's counters of the last RX operation
typedef struct {
  uint16_t rx_ok, rx_nok, rx_ignored, rx_empty, rx_fifo_full, rx_timeout;
  int8_t last_rssi;
} rf_stats_t;
void rf_stats(rf_stats_t *stats);

// Called during rf_rx() at 1/8 steps of the window when set (debugging)
extern void (*rf_debug)(int step, uint32_t events, int8_t rssi, uint16_t endcause);
