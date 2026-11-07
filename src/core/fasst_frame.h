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
// step (27 in one session, 28 in another); the bench's: 27 (the MPM picks
// one). Area France (T14SG): step field FASST_FRANCE_STEP + s (s = 1..22),
// hops k = 1..22 only, the next k + s mod 23, skipping 0
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

// FASST MULT (the T14SG's): the same packet with 16 channels, every
// group's flags select ch1..8 (0) or ch9..16 (1): alt = ch9..16. Plain type
// bytes 12 / 19 = 0x80 | m / 0xA0 | m, m = FASST_MULT_NORMAL (bit 2: DG1 /
// DG2, left 0). F/S cycle, types 1..FASST_MULT_FS_TYPES: type t carries the
// F/S of two channels, ch9 + 13, 10 + 14, 11 + 15, 12 + 16, then 1 + 5 ...
// 4 + 8, in their slots (the other slots as alt), m = code (0, 1, 2, 0x12)
// | mode << 2 per channel: 0 hold (slot = the live value, as the T14SG),
// 1 F/S (slot = fs[c]; plain bit 2 as the TM-10's F/S types). hold bit c
// = channel c holds
#define FASST_MULT_CHANNELS 16
#define FASST_MULT_FS_TYPES 8
#define FASST_MULT_NORMAL 0x13
void fasst_mult_packet(uint8_t* p, uint8_t k, uint8_t step, const uint16_t* ch, bool alt,
                       uint8_t type, const uint16_t* fs, uint16_t hold);

// FASST 7CH (the T14SG's): one 17-byte packet per FASST7_FRAME_US, the
// same hops and step rule. Plain: ID0..2, ID3 (low 5 bits 0) | (k + 1) >>
// 1, byte 4 bit 7 = (k + 1) & 1, then MSB first from byte 4 bit 5: 7 x 11
// bits ch1..7 (value = pulse - 200 us), the tail: step (6 bits), 0, code
// (2 bits); CRC-16/KERMIT over 0..14 at 15..16 (low byte first), all
// whitened with the 10ch stream at offset 46893. Code: normal packet 0
// (throttle on hold) or 1 (throttle F/S set); every FASST7_FS_EVERY-th one
// is an F/S packet, 3 (hold, all live) or 2 (ch3's slot = fs3, its F/S
// position). Only ch3 has F/S in 7CH. fs3 < 0: hold
#define FASST7_PACKET_LEN 17
#define FASST7_CHANNELS 7
#define FASST7_FRAME_US 8000
#define FASST7_FS_EVERY 10
#define FASST7_CHIPS_LEN ((80 + 8 * FASST7_PACKET_LEN) * 11 / 8)
void fasst7_packet(uint8_t* p, uint8_t k, uint8_t step, const uint16_t* ch, bool fs_packet,
                   int16_t fs3);
// A received 7CH packet: as fasst_unpack(), plus the 7 values and the code
bool fasst7_unpack(const uint8_t* p, uint8_t* plain, uint8_t* k, uint8_t* step, uint16_t* ch,
                   uint8_t* code);

// A received packet (FASST_PACKET_LEN bytes as on air): plain = the packet
// unwhitened; true if its CRC is right and its hop field valid, with its hop
// index and step
bool fasst_unpack(const uint8_t* p, uint8_t* plain, uint8_t* k, uint8_t* step);

// The packet's chips (FASST_CHIPS_LEN bytes, MSB first): preamble, sync,
// packet; fasst_spread_len() for len packet bytes ((80 + 8 len) x 11
// chips), fasst_spread_n() with n chips per bit (FASST_BARKER_11 / _7)
void fasst_spread(uint8_t* chips, const uint8_t* p);
void fasst_spread_len(uint8_t* chips, const uint8_t* p, uint32_t len);
void fasst_spread_n(uint8_t* chips, uint8_t n, const uint8_t* p, uint32_t len);

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

// FASSTest TX packet (FASSTEST_PACKET_LEN bytes as on air, 14CH;
// FASSTEST_PACKET_LEN_12CH with ch12) on channel c, as the T14SG sends it:
// type 0 (12CH: 3), hop code `code`, mode 0x13 (DG1 / DG2 off), the 8 slots
// ch1..8 or, with high, ch9..16 (12CH: 5 slots, ch1..5 / ch6..10; ch: the
// 16 / 10 values, 11 bits: the S.BUS value). Plain bytes 2.. whitened with
// 00 00 ID0..5 c ID0..5 c ID0 ID1 (12CH: its first 10 bytes; id: the TX
// ID, 6 bytes, the receiver takes it at the link), CRC-16/KERMIT of the
// plain bytes after them. req 1..FASSTEST_PAGES: a telemetry request (b1
// bit 5) for page req - 1 (14CH: b16 = 1 | page << 1, as the T14SG); the
// receiver answers in the next hop's slot (its channel, sync
// FASSTEST_REPLY_US / _12CH after this packet's), the TX leaving that hop
// out. 0: no request
#define FASSTEST_ID_LEN 6
#define FASSTEST_TX_CHANNELS 16
#define FASSTEST_TX_CHANNELS_12CH 10
#define FASSTEST_PAGES 4
#define FASSTEST_REPLY_US 14565
#define FASSTEST_REPLY_US_12CH 6115
void fasstest_packet(uint8_t* p, bool ch12, const uint8_t* id, uint8_t code, uint8_t c,
                     const uint16_t* ch, bool high, uint8_t req);
// F/S packet for channel n (0..15, 12CH 0..9): its slot and half, the other
// slots of that half from ch; the slot carries value (mode FASSTEST_FS_HOLD:
// the live value, output; F/S, B.F/S: the position, stored, not output).
// Mode field: slot n % 8 (12CH n % 5) as 0x00 / 0x01 / 0x02 / 0x12 for
// slot % 4, + 0x20 for slots 4.., + mode << 2 (T14SG: one per channel,
// all every ~10 s)
#define FASSTEST_FS_HOLD 0
#define FASSTEST_FS_FS 1
#define FASSTEST_FS_BFS 2
void fasstest_fs_packet(uint8_t* p, bool ch12, const uint8_t* id, uint8_t code, uint8_t c,
                        const uint16_t* ch, uint8_t n, uint8_t mode, uint16_t value);
// A telemetry reply (type 0x10, 12CH 0x13; FASSTEST_PACKET_LEN / _12CH
// bytes as on air) heard on channel c: plain = its bytes unwhitened with rx
// (the receiver's serial, 4 bytes) in place of ID0..3; true if the CRC is
// right. 14CH: plain b1 bits 4..5 = the page, bytes 2 + 2i, 3 + 2i = S.BUS2
// slot 8 page + i (its 2 data bytes; slot 0 = the receiver's voltage, b2
// bits 6..7 the link quality 0..3). 12CH: bytes 2..3 the receiver's
// voltage (as slot 0), byte 4 the link quality %, the rest 0
bool fasstest_reply_unpack(const uint8_t* p, bool ch12, uint8_t* plain, const uint8_t* rx,
                           const uint8_t* id, uint8_t c);
// A reply as the receiver sends it (fasstest_reply_unpack() reads it):
// the TX's hop code code, page (14CH, 0..FASSTEST_PAGES - 1), data = plain
// bytes 2.. (16, 12CH 8), rx its serial, on channel c
void fasstest_reply_packet(uint8_t* p, bool ch12, uint8_t code, uint8_t page, const uint8_t* data,
                           const uint8_t* rx, const uint8_t* id, uint8_t c);
// The receiver's serial from a reply (as fasstest_reply_unpack()) without
// a link: 14CH, bytes 9..12 or 2..5 are the serial when those plain bytes
// are 0 (empty slots), checked by the CRC; 12CH, bytes 9 and 5 (plain 0),
// the other two solved from the CRC: confirm it on another reply. False if
// no candidate
bool fasstest_reply_serial(const uint8_t* p, bool ch12, const uint8_t* id, uint8_t c, uint8_t* rx);
// FASSTest link packet (type 8, not whitened, 11 chips and 20 bytes in
// both modes) for channel c: the hop code of the row the data will use,
// the TX ID, the battery F/S voltage (0.1 V), the channel (b9: the
// receiver checks it), b14 bit 1 = 12CH, b15 = 2. The T14SG's LINK (a few
// seconds, nothing else sent) puts one on each channel of the receiver's
// link scan list 2, 8 ... 44 (FASSTEST_LINK_CHANNELS) every
// FASSTEST_LINK_EVERY hops, FASSTEST_LINK_GAP hops apart, until the reply.
// A receiver within its link window (2..5 s after power-up, R7206SB
// firmware) takes the ID, saves it at once and answers on the same channel
// (type 0x18 FASSTEST_LINK_REPLY_US after: f8 00, the packet's bytes 2..9,
// its serial, b14, b15). It saves ID0..3 as 9 decimal digits: an ID0..3
// above 999999999 comes back truncated after a power cycle
#define FASSTEST_LINK_CHANNELS 8
#define FASSTEST_LINK_CHANNEL(i) (2 + 6 * (i))
#define FASSTEST_LINK_EVERY 30
#define FASSTEST_LINK_GAP 4
#define FASSTEST_LINK_REPLY_US 14900
#define FASSTEST_TYPE_LINK_REPLY 0x18
#define FASSTEST_BFS_DEFAULT 38  // 3.8 V
void fasstest_link_packet(uint8_t* p, bool ch12, const uint8_t* id, uint8_t code, uint8_t c,
                          uint8_t bfs);
// A link packet heard on channel c: true if its type, CRC and channel are
// right (the TX ID in bytes 2..7, the hop code as a TX packet's, 12CH b14
// bit 1)
bool fasstest_link_check(const uint8_t* p, uint8_t c);
// The receiver's answer to link packet link (FASSTEST_PACKET_LEN bytes,
// not whitened), rx its serial
void fasstest_link_reply(uint8_t* p, const uint8_t* link, const uint8_t* rx);
// Their chips: fasst_spread_n() of the 20 (12CH: 12) bytes
#define FASSTEST_CHIPS_LEN 330
#define FASSTEST_CHIPS_TX_LEN (FASSTEST_CHIPS_LEN - 4)
#define FASSTEST_CHIPS_LEN_12CH 154
#define FASSTEST_CHIPS_TX_LEN_12CH (FASSTEST_CHIPS_LEN_12CH - 4)

// b bits from chips (MSB first), n chips each (FASST_BARKER_11 / _7) from
// chip `first`: bit i to out[i / 8] bit i % 8 (LSB first, as on air),
// corr[i] = |correlation| with the Barker code, 0..n (n: all chips right,
// n - 2: one chip off, 1: noise)
void fasst_despread(const uint8_t* chips, uint8_t n, uint32_t first, uint32_t b, uint8_t* out,
                    uint8_t* corr);

// A received packet (radio_rx_start(FASST_RX_PKT_LEN(n, bytes)) with
// fasst_rx_sync_word(n)): the first `bytes` packet bytes to out, per byte
// its worst bit correlation to q (or NULL); the bits from the RF core's
// sync word to the real one (the sync bits left: next, or up to
// FASST_RX_EARLY_BITS later), -1 if not found. The RF core also finds its
// sync word at the very start of a packet (the sender's synthesizer
// settling; the sync bits 6..15 came 68 bits later): the RX takes the
// whole preamble too
#define FASST_RX_EARLY_BITS 96
#define FASST_RX_BITS(n, bytes) (FASST_RX_SYNC_LEFT(n) + FASST_RX_EARLY_BITS + 8 * (bytes))
#define FASST_RX_PKT_LEN(n, bytes) FASST_RX_LEN(n, FASST_RX_BITS(n, bytes))
// FASST_RX_LEN() within RADIO_MAX_PKT_LEN, 2 bytes spare for the despreader
// (the 11-chip limits, the 7-chip mode taking the same)
#define FASST_RX_BITS_MAX ((8 * (RADIO_MAX_PKT_LEN - 2) - FASST_RX_SKIP_CHIPS(11)) / 11)
#define FASST_RX_BYTES_MAX ((FASST_RX_BITS_MAX - FASST_RX_SYNC_LEFT(11) - FASST_RX_EARLY_BITS) / 8)
int32_t fasst_rx_bytes(const uint8_t* data, uint8_t n, uint16_t bytes, uint8_t* out, uint8_t* q);
