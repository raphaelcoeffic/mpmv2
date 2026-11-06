#pragma once

// Futaba FASST 10ch framing (docs/mpm-core.md; the tracing notes and a
// Python reference encoder in doc/futaba*, not in the repository). One
// 22-byte packet per 7 ms hop; each bit is sent as 11 Barker chips, the RF
// core sending the chips as 2-FSK symbols. Its own preamble byte goes out
// first, then the first 32 chips as its sync word (fasst_sync_word()),
// then the rest (fasst_chips()).
//
// The packet is whitened by a fixed LFSR stream, its CRC included: the
// same for every transmitter. Per transmitter: the ID (fasst_set_id()),
// and the hop step the packets carry.

#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

#define FASST_FRAME_US 7000
#define FASST_CHANNELS 10
#define FASST_PACKET_LEN 22
// 64 preamble + 16 sync + 176 payload bits, 11 chips each, 8 per byte
#define FASST_CHIPS_LEN 352
#define FASST_HOPS 36

// Channel values: 11 bits, pulse = 880 + 0.625 * value us (1024 = 1520 us)
#define FASST_VALUE_MAX 2047

// The F/S cycle: packet types 1..6 (fasst_packet()), one per frame, in
// order; one cycle sets the receiver's failsafe. 0 = normal packet
#define FASST_FS_TYPES 6

// Bytes 0..3 of the plain packet; on air whitened (fasst_packet()). The
// TM-10's: 12 68 9f e0 (on air 12 a8 d3 0d). A receiver links to any (an
// R6008HS took 12 5a 3c 71); byte 0 is 0x12 in both TM-10 modes, its
// meaning unknown
#define FASST_ID_DEFAULT 0x12689FE0
#define FASST_ID_BYTE0 0x12
void fasst_set_id(uint32_t id);

// PHY: 1.5 Mchip/s (the R6008HS takes 1.46..1.545), 500 kHz deviation
// (flat 200..512 kHz), 2-FSK
void fasst_phy(radio_phy_t* phy, uint16_t tx_power);

// Hop index k (0..35): the first, the next (k + step mod 37, skipping 36;
// the packets carry the step), and its frequency. The TM-10 chooses its
// step (27 in one session, 28 in another); ours: 27
#define FASST_HOP_FIRST 0
#define FASST_HOP_STEP 27
uint8_t fasst_next_hop(uint8_t k, uint8_t step);
uint32_t fasst_freq_khz(uint8_t k);

// One packet for hop k and the hop step. ch: the 10 channel values. type
// 0: normal; with alt, ch9 / ch10 instead of ch1 / ch2 (each pair goes out
// every other frame). type 1..FASST_FS_TYPES: the F/S cycle, fs = the 10 F/S values,
// hold bit i = channel i holds (NOR) instead of going to fs[i]
void fasst_packet(uint8_t* p, uint8_t k, uint8_t step, const uint16_t* ch, bool alt, uint8_t type,
                  const uint16_t* fs, uint16_t hold);

// A received packet (FASST_PACKET_LEN bytes as on air): plain = the packet
// unwhitened; true if its CRC is right and its hop field valid, with its hop
// index and step
bool fasst_unpack(const uint8_t* p, uint8_t* plain, uint8_t* k, uint8_t* step);

// The packet's chips (FASST_CHIPS_LEN bytes, MSB first): preamble, sync,
// packet
void fasst_spread(uint8_t* chips, const uint8_t* p);

// The RF core's sync word: the first 32 chips. The packet: the rest
uint32_t fasst_sync_word(const uint8_t* chips);
#define fasst_chips(chips) ((chips) + 4)
#define FASST_CHIPS_TX_LEN (FASST_CHIPS_LEN - 4)

// Reception: the RF core's RX sync word is the first 32 chips of sync bits
// 3..5 (0 1 1). Bits 1..3 (0 0 0) also matched at the very start of the
// TM-10's packets, the demodulator then settled wrong for the whole packet.
// After the sync word: FASST_RX_SKIP_CHIPS chips (the rest of bit 5), the
// FASST_RX_SYNC_LEFT sync bits 6..15, then the packet
uint32_t fasst_rx_sync_word();
#define FASST_RX_SKIP_CHIPS 1
#define FASST_RX_SYNC_LEFT 10
uint16_t fasst_rx_sync_left();  // sync bits 6..15, bit 0 first
// RX length in bytes for n bits after the sync word (sync bits 6..15
// included)
#define FASST_RX_LEN(n) ((FASST_RX_SKIP_CHIPS + 11 * (n) + 7) / 8)

// FASSTest (doc/futaba.md "Hop sequence", "T14SG on the air"): same PHY,
// sync and Barker chips, one packet per 15 ms hop on a 1.024 MHz grid,
// channel c = 0..75. Bytes 0..1 go out in the clear: packet type (byte 0
// bits 0..4, 0x10 = the receiver's reply) and a 7-bit hop code (one of 46
// codewords: set A rows 0..22 on channels 2, 5 ... 68, set B rows 0..22 on
// 2, 4 ... 46; each row visits 22 of its 23 channels)
#define FASSTEST_FRAME_US 15000
#define FASSTEST_CHANNELS 76
#define FASSTEST_HOPS 22
#define FASSTEST_PACKET_LEN 20  // 18 bytes + CRC (the 14CH mode)
#define FASSTEST_TYPE_REPLY 0x10
uint32_t fasstest_freq_khz(uint8_t c);
// The hop row from a packet's bytes 0..1: false for an unknown hop code
bool fasstest_hop_code(const uint8_t* p, uint8_t* set, uint8_t* row);
// The channel after c in the row; false if c isn't in it
bool fasstest_next_channel(uint8_t set, uint8_t row, uint8_t c, uint8_t* next);

// n bits from chips (MSB first), 11 chips each from chip `first`: bit i to
// out[i / 8] bit i % 8 (LSB first, as on air), corr[i] = |correlation| with
// the Barker code, 0..11 (11: all chips right, 1: noise)
void fasst_despread(const uint8_t* chips, uint32_t first, uint32_t n, uint8_t* out, uint8_t* corr);
