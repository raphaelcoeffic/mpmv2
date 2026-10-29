// CC27xx radio: TI's LRF layer for the setup (PHY, trims, synthesizer, TX
// power), the PBE's generic TX and RX operations driven directly, blocking.
// Raw framing: preamble, a sync word of up to 32 bits, a fixed length
// payload; no header, length field or CRC (done in software, as the CC2500
// and NRF24 emulations do).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <ti/drivers/rcl/LRF.h>

#define RF_MAX_PAYLOAD 64

// Operation results: the PBE's end cause, or one of these
#define RF_OK 0
#define RF_ERR_SETUP -1     // LRF_setupRadio() failed
#define RF_ERR_TIMEOUT -2   // no OPDONE / OPERROR in time
#define RF_ERR_NO_PACKET -3 // RX: nothing in the window

void rf_init(void);

// PHY setup (a generated LRF_Config, see tools/cc27_phy.py), then the raw
// framing on top of it. Returns LRF_SetupResult (0: ok)
int rf_setup(const LRF_Config *config);

// DEMMISC3 (demodulator decimation) for TX and for RX, when they differ:
// the PDIF decimation slows the modulator too. 0: the PHY's. rf_setup()
// clears them
void rf_set_demmisc3(uint16_t tx, uint16_t rx);

// Sync word, MSB first as on air, bits (8 to 32) right-aligned
void rf_set_sync(uint32_t sync, uint8_t bits);

// TX power in dBm (from the PA table: the next level up)
int rf_set_power(int8_t dbm);

int rf_tx(uint32_t freq_hz, const uint8_t *data, uint8_t len);

typedef struct {
  uint8_t len;
  int8_t rssi;
  uint32_t time;  // SYSTIM at sync (0.25 us)
  uint8_t data[RF_MAX_PAYLOAD];
} rf_packet_t;

// Receive one packet of len bytes (2 at least) within timeout_us
int rf_rx(uint32_t freq_hz, uint8_t len, uint32_t timeout_us, rf_packet_t *pkt);

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
