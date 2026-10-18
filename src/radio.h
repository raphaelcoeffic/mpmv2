#pragma once

#include <stdbool.h>
#include <stdint.h>

// Raw 2-GFSK / 2-FSK packet radio on top of the RF core proprietary mode:
// preamble + sync word + fixed length payload, no HW CRC and no whitening.
// Protocol framing (address bytes, CRC, scrambling) is left to the caller.

// Packets of up to 255 bytes use the basic RF core commands, longer ones the
// advanced ones (CYRF6936 SDR emulation: 1 KB for 16 bytes)
#define RADIO_MAX_PKT_LEN 1100

typedef struct {
  uint32_t symbol_rate;   // bps
  uint32_t deviation;     // Hz, multiple of 250
  uint8_t rx_bw;          // RF core RX bandwidth code (64..103)
  uint8_t preamble_bytes; // 1..32
  uint8_t sync_bits;      // 8..32
  int16_t if_freq;        // MHz in 4.12 signed format, 0x8000 = default
  uint16_t tx_power;      // RF core TX power word
  uint8_t sync_threshold; // sync word correlation threshold, 0 = default (0x1F)
  bool fsk;               // 2-FSK instead of 2-GFSK
} radio_phy_t;

typedef struct {
  uint8_t data[RADIO_MAX_PKT_LEN];
  uint16_t len;
  int8_t rssi;  // dBm
  uint32_t timestamp_us;
} radio_packet_t;

typedef enum {
  RADIO_OK = 0,
  RADIO_ERR_BOOT,
  RADIO_ERR_CMD,
  RADIO_ERR_PARAM,
  RADIO_ERR_TIMEOUT,
  RADIO_NO_PACKET,
} radio_status_t;

// Power up the RF core and apply the PHY configuration
radio_status_t radio_init(const radio_phy_t* phy);

// Power down the RF core
void radio_off();

// Tune the synthesizer: frequency in kHz
radio_status_t radio_set_frequency(uint32_t freq_khz);

// Sync words for TX and RX, right aligned on sync_bits
void radio_set_sync_word(uint32_t tx_sync, uint32_t rx_sync);

// Alternative RX sync word for radio_rx_start(), 0 = none
void radio_set_rx_sync_alt(uint32_t rx_sync);

// Send one packet, blocks until sent
radio_status_t radio_send(const uint8_t* data, uint16_t len);

// Start continuous reception of fixed length packets
radio_status_t radio_rx_start(uint16_t len);

// Send one packet, then receive one packet of rx_len bytes with the rx_sync
// sync word (e.g. an acknowledgment). RX starts right after TX (chained RF core
// commands) and lasts window_us. Returns RADIO_NO_PACKET if nothing was received.
radio_status_t radio_send_rx(const uint8_t* data, uint16_t len, uint32_t rx_sync,
                             uint16_t rx_len, uint32_t window_us, radio_packet_t* pkt);

// Stop reception
void radio_rx_stop();

// True while the RX command runs (it ends on errors, e.g. RX queue full)
bool radio_rx_active();

// RF core receive counters of the current/last RX command
typedef struct {
  uint16_t ok;        // packets received
  uint16_t nok;       // packets with errors
  uint16_t buf_full;  // packets dropped, RX queue full
} radio_rx_stats_t;

void radio_rx_stats(radio_rx_stats_t* stats);

// Returns true and fills pkt if a packet has been received
bool radio_receive(radio_packet_t* pkt);

// Last RF core command status, for diagnostics
uint16_t radio_last_status();
