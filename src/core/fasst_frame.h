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
// step (27 in one session, 28 in another); ours: 27. Area France (T14SG):
// step field FASST_FRANCE_STEP + s (s = 1..22), hops k = 1..22 only, the
// next k + s mod 23, skipping 0
#define FASST_HOP_FIRST 0
#define FASST_HOP_STEP 27
#define FASST_FRANCE_STEP 40
#define FASST_FRANCE_HOPS 22
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
// packet; fasst_spread_len() for len packet bytes ((80 + 8 len) x 11 chips)
void fasst_spread(uint8_t* chips, const uint8_t* p);
void fasst_spread_len(uint8_t* chips, const uint8_t* p, uint32_t len);

// The RF core's sync word: the first 32 chips. The packet: the rest
uint32_t fasst_sync_word(const uint8_t* chips);
#define fasst_chips(chips) ((chips) + 4)
#define FASST_CHIPS_TX_LEN (FASST_CHIPS_LEN - 4)

// Reception, n chips per bit (FASST_BARKER_11, or FASST_BARKER_7 for
// FASSTest 12CH: Barker-7 1110010, the R7206SB's 7-chip mode): the RF core's
// RX sync word is the first 32 chips from sync bit 3 (11 chips: bits 3..5 =
// 0 1 1; 7: bits 3..7). Bits 1..3 (0 0 0) also matched at the very start of
// the TM-10's packets, the demodulator then settled wrong for the whole
// packet. After the sync word: FASST_RX_SKIP_CHIPS(n) chips (the rest of
// its last bit), the FASST_RX_SYNC_LEFT(n) sync bits left (11 chips: 6..15),
// then the packet
#define FASST_BARKER_11 11
#define FASST_BARKER_7 7
#define FASST_RX_SYNC_BITS(n) ((32 + (n) - 1) / (n))
#define FASST_RX_SKIP_CHIPS(n) (FASST_RX_SYNC_BITS(n) * (n) - 32)
#define FASST_RX_SYNC_LEFT(n) (13 - FASST_RX_SYNC_BITS(n))
uint32_t fasst_rx_sync_word(uint8_t n);
uint16_t fasst_rx_sync_left(uint8_t n);  // the sync bits left, bit 0 first
// RX length in bytes for b bits after the sync word (the sync bits left
// included)
#define FASST_RX_LEN(n, b) ((FASST_RX_SKIP_CHIPS(n) + (n) * (b) + 7) / 8)

// FASSTest (doc/futaba.md "Hop sequence", "T14SG on the air"): same PHY
// and sync, Barker-11 chips (14CH) or Barker-7 (12CH, FASST_BARKER_7), one
// packet per 15 ms (14CH) or 6.3 ms (12CH) hop on a 1.024 MHz grid,
// channel c = 0..75. Bytes 0..1 go out in the clear: packet type (byte 0
// bits 0..4: 0 / 3 data in 14CH / 12CH, 0x10 / 0x13 the receiver's reply)
// and a 7-bit hop code (one of 46 codewords: set A rows 0..22 on channels
// 2, 5 ... 68, set B rows 0..22 on 2, 4 ... 46; each row visits 22 of its
// 23 channels)
#define FASSTEST_FRAME_US 15000
#define FASSTEST_FRAME_US_12CH 6300  // 22 hops in 138.6 ms (T14SG)
#define FASSTEST_CHANNELS 76
#define FASSTEST_HOPS 22
#define FASSTEST_PACKET_LEN 20       // 18 bytes + CRC (14CH)
#define FASSTEST_PACKET_LEN_12CH 12  // 10 bytes + CRC (12CH, 7 chips per bit)
#define FASSTEST_TYPE_REPLY 0x10     // with bits 0..1 masked: 0x10, 0x13
uint32_t fasstest_freq_khz(uint8_t c);
// The hop row from a packet's bytes 0..1: false for an unknown hop code
bool fasstest_hop_code(const uint8_t* p, uint8_t* set, uint8_t* row);
// The channel after c in the row; false if c isn't in it
bool fasstest_next_channel(uint8_t set, uint8_t row, uint8_t c, uint8_t* next);
// Hop i (0..21) of a row, and the row's hop code
uint8_t fasstest_channel(uint8_t set, uint8_t row, uint8_t i);
uint8_t fasstest_code(uint8_t set, uint8_t row);

// FASSTest 14CH TX packet (FASSTEST_PACKET_LEN bytes as on air) on channel
// c, as the T14SG sends it: type 0, hop code `code`, mode 0x13 (DG1 / DG2
// off), the 8 slots ch1..8 or, with high, ch9..16 (ch: 16 values, 11 bits:
// the S.BUS value), no telemetry request. Plain bytes 2..17 whitened with
// 00 00 ID0..5 c ID0..5 c ID0 ID1 (id: the TX ID, 6 bytes; the receiver
// takes it at the link), CRC-16/KERMIT of the plain 18 bytes after them
#define FASSTEST_ID_LEN 6
#define FASSTEST_TX_CHANNELS 16
void fasstest_packet(uint8_t* p, const uint8_t* id, uint8_t code, uint8_t c, const uint16_t* ch,
                     bool high);
// Its chips: fasst_spread_len() of the 20 bytes
#define FASSTEST_CHIPS_LEN 330
#define FASSTEST_CHIPS_TX_LEN (FASSTEST_CHIPS_LEN - 4)

// b bits from chips (MSB first), n chips each (FASST_BARKER_11 / _7) from
// chip `first`: bit i to out[i / 8] bit i % 8 (LSB first, as on air),
// corr[i] = |correlation| with the Barker code, 0..n (n: all chips right,
// n - 2: one chip off, 1: noise)
void fasst_despread(const uint8_t* chips, uint8_t n, uint32_t first, uint32_t b, uint8_t* out,
                    uint8_t* corr);
