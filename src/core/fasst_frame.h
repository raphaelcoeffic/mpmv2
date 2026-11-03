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
// the packets carry the step), and its frequency. The TM-10's step: 27
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

// The packet's chips (FASST_CHIPS_LEN bytes, MSB first): preamble, sync,
// packet
void fasst_spread(uint8_t* chips, const uint8_t* p);

// The RF core's sync word: the first 32 chips. The packet: the rest
uint32_t fasst_sync_word(const uint8_t* chips);
#define fasst_chips(chips) ((chips) + 4)
#define FASST_CHIPS_TX_LEN (FASST_CHIPS_LEN - 4)
