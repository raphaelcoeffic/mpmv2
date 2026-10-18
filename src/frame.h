#pragma once

#include <stdbool.h>
#include <stdint.h>

// On-air packet formats of the MULTI-Module 2.4 GHz chips (NRF24L01, XN297,
// CC2500), emulated on top of the raw radio (preamble + sync word + fixed
// length data, see radio.h).
//
// A frame is: header (bytes known in advance: XN297 preamble, address, CC2500
// last 2 preamble bytes and sync word) | encoded payload | CRC. TX sends the
// first header bytes as the sync word and the rest as data. RX syncs on the
// bytes known in advance (NRF preamble, header and payload prefix) that differ
// the most from the bits before them: repeated patterns (e.g. address
// e7e7e7e7e7) make the CC2652 miss packets.

#define FRAME_ADDR_MAX 5
#define FRAME_PREFIX_MAX 8
#define FRAME_PAYLOAD_MAX 48
#define FRAME_NRF_PAYLOAD_MAX 32  // NRF and XN297
#define FRAME_HDR_MAX (4 + FRAME_ADDR_MAX)

typedef enum {
  FRAME_RAW,    // sync word + payload
  FRAME_NRF,    // NRF24L01 ShockBurst, or Enhanced ShockBurst (esb)
  FRAME_XN297,  // XN297, standard or enhanced mode
  FRAME_CC2500, // CC2500 packet: preamble, sync word, payload, CRC
} frame_type_t;

typedef struct {
  frame_type_t type;
  // Address as given to the NRF24L01 TX_ADDR register or XN297_SetTXAddr()
  // (sent last byte first), 3 to 5 bytes. Raw: the sync word, MSB first.
  // CC2500: SYNC1, SYNC0, twice (4 bytes) for MDMCFG2.SYNC_MODE 3 or 7.
  uint8_t addr[FRAME_ADDR_MAX];
  uint8_t addr_len;
  uint8_t crc_len;     // NRF: 0 to 2, XN297 and CC2500: 0 or 2
  // NRF: 9 bit packet control field, sent by the nRF24L01+ when EN_AA or
  // dynamic payload length (DYNPD) is enabled
  bool esb;
  bool dpl;            // NRF esb: dynamic payload length
  bool scramble;       // XN297
  bool enhanced;       // XN297 enhanced mode (packet control field)
  bool noack;          // NRF esb / XN297 enhanced mode: no ack request
  // CC2500: variable packet length (PKTCTRL0.LENGTH_CONFIG 1), the first
  // payload byte is the length of the rest (CRC excluded)
  bool var_len;
  uint8_t sync_bytes;  // NRF: address bytes used as sync word (2 to 4)
  // Known first payload bytes (not with a packet control field)
  uint8_t prefix[FRAME_PREFIX_MAX];
  uint8_t prefix_len;

  // Set by frame_init()
  uint8_t known[FRAME_HDR_MAX + FRAME_PREFIX_MAX];  // over the air
  uint8_t hdr_len;
  uint8_t known_len;
  uint8_t sync_len;
  uint32_t rx_sync;        // RX sync word, see frame.c
  uint8_t rx_sync_errors;  // bit errors from the bits before it (max 8)
  uint8_t rx_start;        // frame byte after the RX sync word
  uint8_t rx_first_start;  // earliest one if the sync word repeats
  uint8_t pid;     // packet id of the next packet (packet control field)
  uint8_t tx_pid;  // packet id of the last packet encoded
} frame_t;

typedef enum { FRAME_SHORT, FRAME_BAD, FRAME_OK } frame_result_t;

// Checks the configuration and computes the known bytes
bool frame_init(frame_t* f);

// Sync word size in bits and values (MSB first)
uint8_t frame_sync_bits(const frame_t* f);
uint32_t frame_tx_sync(const frame_t* f);
uint32_t frame_rx_sync(const frame_t* f);

// True if the RX sync word is too close to the bits sent before it (repeated
// patterns, e.g. address 6565656565): use a stricter sync threshold
bool frame_rx_sync_weak(const frame_t* f);

// True if a payload of len bytes can be sent / received
bool frame_len_ok(const frame_t* f, uint8_t len);

// Encodes a packet: data to send after the TX sync word.
// Returns its length, 0 if the payload is too long.
uint8_t frame_tx_data(frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* data);

// Bytes to receive after the RX sync word for a payload of len bytes
// (packet control field with a length: maximum length), 0 if not possible
uint8_t frame_rx_len(const frame_t* f, uint8_t len);

// Decodes data received after the RX sync word (rx_len bytes). len is the
// payload length (packet control field with a length: maximum length). Fills
// payload and payload_len, FRAME_OK if the CRC (if any) and known bytes are right.
frame_result_t frame_decode(const frame_t* f, const uint8_t* data, uint8_t rx_len,
                            uint8_t len, uint8_t* payload, uint8_t* payload_len);

// Same, also returns the NRF packet control field (0 for other framings)
frame_result_t frame_decode_pcf(const frame_t* f, const uint8_t* data, uint8_t rx_len,
                                uint8_t len, uint8_t* payload, uint8_t* payload_len,
                                uint16_t* pcf);
