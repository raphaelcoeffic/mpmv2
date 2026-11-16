#pragma once

// Jeti Duplex 2.4 GHz framing (doc/jeti.md; the tracing notes and Python
// tools in doc/jeti/, not in the repository). IEEE 802.15.4 O-QPSK at
// 250 kbps, which is MSK at 2 Mchip/s: sent and received as 2 Mbps
// symbols, preamble symbol 0 as the sync word (JETI_SYNC), the rest of the
// PPDU spread on the CPU (jeti_spread(), jeti_despread()).
//
// MAC header: FCF 0x0801 (data, PAN ID compression off, short destination,
// no source), seq, destination PAN and short address (the receiver's, or
// broadcast at bind). The payload's byte 0 (type) goes out plain, the rest
// scrambled (jeti_scramble()); FCS as 802.15.4.

#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

#define JETI_SYNC 0xE077AE6C
#define JETI_PSDU_MAX 127
#define JETI_HDR 7         // MAC header: FCF, seq, PAN, dst
#define JETI_TYPE JETI_HDR  // payload byte 0, plain
#define JETI_FCS 2
#define JETI_SYMBOL_US 16
#define JETI_PREAMBLE_SYMBOLS 8
#define JETI_CHANNELS 16  // IEEE channels 11..26: 2405 + 5 n MHz, n = 0..15

// Air bytes after the sync word: the other 7 preamble symbols, SFD, PHR,
// the PSDU of len bytes (FCS included), 4 bytes per symbol (+ 2 trailing
// bytes on TX)
#define JETI_AIR_LEN(len) ((11 + 2 * (len)) * 4)
#define JETI_TX_LEN(len) (JETI_AIR_LEN(len) + 2)
// Air time from the sync word's start to the last PSDU bit
#define JETI_AIR_US(len) ((12 + 2 * (len)) * JETI_SYMBOL_US)

// The PHY: 2 Mbps MSK (on the CC2652: its Bluetooth LE 2M PHY), sync word
// JETI_SYNC both ways (radio_set_sync_word() after radio_init())
void jeti_phy(radio_phy_t* phy, uint16_t tx_power);

// Channel n (0..15, the frames' hop list nibbles) in kHz
static inline uint32_t jeti_freq_khz(uint8_t n) { return 2405000 + 5000 * n; }

// The MAC header (JETI_HDR bytes) into f
void jeti_header(uint8_t* f, uint8_t seq, uint16_t pan, uint16_t dst);

// 802.15.4 FCS (CRC-16/KERMIT)
uint16_t jeti_crc16(const uint8_t* p, uint8_t n);
// The PSDU's last 2 bytes are its FCS
bool jeti_fcs_ok(const uint8_t* psdu, uint8_t len);

// Payload scrambler: a key register fed with every plain byte (the MAC
// header's too), payload bytes from 1 on sent as plain ^ key. n: PSDU bytes
// without the FCS. In place allowed
void jeti_scramble(const uint8_t* plain, uint8_t* out, uint8_t n);
void jeti_descramble(const uint8_t* in, uint8_t* out, uint8_t n);

// Spread the PSDU (len bytes, FCS not included: appended here) into air:
// JETI_TX_LEN(len + JETI_FCS) bytes, returned
uint16_t jeti_spread(uint8_t* air, const uint8_t* psdu, uint8_t len);

// Despread n air bytes after the sync word into psdu: the preamble's
// remaining symbols, SFD (7, A), PHR, the PSDU. Returns the PSDU bytes the
// capture holds, -1 without an SFD; *len: the PHR's length, *pre: preamble
// symbols after the sync word, *dmax: the worst symbol's chip distance
int jeti_despread(const uint8_t* d, uint16_t n, uint8_t* psdu, uint8_t* len, uint8_t* pre,
                  uint8_t* dmax);
