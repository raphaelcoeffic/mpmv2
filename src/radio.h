#pragma once

#include <stdbool.h>
#include <stdint.h>

// Raw 2-GFSK / 2-FSK packet radio on top of the RF core proprietary mode:
// preamble + sync word + fixed length payload, no HW CRC and no whitening.
// Protocol framing (address bytes, CRC, scrambling) is left to the caller.

// Packets of up to 255 bytes use the basic RF core commands, longer ones the
// advanced ones (CYRF6936 SDR emulation: 1 KB for 16 bytes). CC26x0 (20 KB of
// RAM): no CYRF6936 emulation, 255 bytes
#if defined(MCU_CC26X0)
#define RADIO_MAX_PKT_LEN 255
#else
#define RADIO_MAX_PKT_LEN 1100
#endif

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
  // Bluetooth LE 2M PHY (2 Mbps GFSK, 500 kHz deviation, 32 bit sync word)
  // under the same packet commands: symbol_rate, deviation, rx_bw,
  // preamble_bytes, if_freq and fsk are ignored
  bool ble_2m;
} radio_phy_t;

typedef struct {
  uint8_t data[RADIO_MAX_PKT_LEN];
  uint16_t len;
  int8_t rssi;  // dBm
  // micros() when the RF core found the sync word: its end, plus the
  // demodulator delay. The packet reaches radio_receive() 20 bit times + 40
  // us after its last bit (proprietary PHY, measured)
  uint32_t timestamp_us;
  // Sender's carrier above the receive frequency, kHz: the modem's estimate
  // (+-20% and about 2 kHz of noise), RADIO_FREQ_OFFSET_NONE if not known
  // (single packet RX of radio_send_rx())
  int16_t freq_offset;
} radio_packet_t;

#define RADIO_FREQ_OFFSET_NONE INT16_MIN

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

// Same, without waiting: the RF core tunes (CMD_FS, about 140 us) while the
// CPU goes on. The next radio call waits for it first (radio_tx_wait())
radio_status_t radio_set_frequency_start(uint32_t freq_khz);

// Sync words for TX and RX, right aligned on sync_bits
void radio_set_sync_word(uint32_t tx_sync, uint32_t rx_sync);

// Alternative RX sync word for radio_rx_start(), 0 = none
void radio_set_rx_sync_alt(uint32_t rx_sync);

// Send one packet, blocks until sent
radio_status_t radio_send(const uint8_t* data, uint16_t len);

// Start sending one packet and return. data must stay valid until the end:
// radio_tx_done() or radio_tx_wait(). The other calls wait for it first
radio_status_t radio_tx_start(const uint8_t* data, uint16_t len);

// Same, the RF core starting the command at micros() time at_us (0 = now,
// in the past = now), within about 500 s: the PA turns on 102 us later
radio_status_t radio_tx_start_at(const uint8_t* data, uint16_t len, uint32_t at_us);

// True once the packet of radio_tx_start() is sent (or failed)
bool radio_tx_done();

// Wait for the end of radio_tx_start()'s packet: its status
radio_status_t radio_tx_wait();

// Start continuous reception of fixed length packets
radio_status_t radio_rx_start(uint16_t len);

// Variable length RX: the length of each packet comes from its first bytes.
// Called from the RF core interrupt with the first n >= hdr_len bytes (after
// the sync word): the packet length, 0 to drop the packet
typedef uint16_t (*radio_len_fn_t)(const uint8_t* data, uint16_t n, void* ctx);

// Start continuous reception of packets of up to max_len bytes, their length
// given by len_fn() after hdr_len bytes (1 to 32). The RF core takes the
// length about 100 us after the sync word at the earliest: a shorter packet
// ends there. One packet per RF core command: RX starts again after each
// packet (as radio_rx_start(), about 70 us to RX on). The Bluetooth LE PHY:
// max_len bytes, as radio_rx_start()
radio_status_t radio_rx_start_var(uint8_t hdr_len, uint16_t max_len, radio_len_fn_t len_fn,
                                  void* ctx);

// Send one packet, then receive one packet of rx_len bytes with the rx_sync
// sync word (e.g. an acknowledgment). RX starts right after TX (chained RF core
// commands) and lasts window_us. Returns RADIO_NO_PACKET if nothing was received.
radio_status_t radio_send_rx(const uint8_t* data, uint16_t len, uint32_t rx_sync,
                             uint16_t rx_len, uint32_t window_us, radio_packet_t* pkt);

// Stop reception
void radio_rx_stop();

// True while the RX command runs (it ends on errors, e.g. RX queue full)
bool radio_rx_active();

// TX power word for the next packets (CMD_SET_TX_POWER), without a new
// radio_init(): power per packet (LBT). radio_init() sets phy->tx_power again
radio_status_t radio_set_tx_power(uint16_t tx_power);

// Instant RSSI in dBm while RX runs, RADIO_RSSI_NONE if not (yet) valid
#define RADIO_RSSI_NONE (-128)
int8_t radio_rssi();

// RF core receive counters of the current/last RX command
typedef struct {
  uint16_t ok;        // packets received
  uint16_t nok;       // packets with errors
  uint16_t buf_full;  // packets dropped, RX queue full
} radio_rx_stats_t;

void radio_rx_stats(radio_rx_stats_t* stats);

// Returns true and fills pkt if a packet has been received
bool radio_receive(radio_packet_t* pkt);

// Variable length RX: the packet being received, as soon as its last byte is
// in, before the RF core ends it (radio_receive() gets it 20 bit times + 40
// us after its last bit): the RSSI sampled by the calls while it came in
// (else read now), timestamp_us = micros() now, the frequency offset from its
// first bytes (CC27xx: none). radio_receive() does not return it
// again. Call radio_receive() first: the packets before it
bool radio_receive_early(radio_packet_t* pkt);

// Extra RF core override words (HW_REG_OVERRIDE() etc.), appended to the
// setup overrides of the next radio_init(): register experiments. n <= 8
#define RADIO_EXTRA_OVERRIDES 8
void radio_set_extra_overrides(const uint32_t* words, uint8_t n);

// Last RF core command status, for diagnostics
uint16_t radio_last_status();

// HF clock correction given to the RF core (CC2652RB HPOSC), 0 otherwise:
// F_nominal = F_hposc * (1 + offset / 2^22), -411 = HPOSC 98 ppm fast
int16_t radio_hposc_offset();
