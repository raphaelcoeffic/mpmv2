// Futaba FASST 10ch framing (fasst_frame.h)
#include "fasst_frame.h"

#include <string.h>

#define PREAMBLE_BITS 64
#define SYNC 0x2BB1  // bit 0 first: B1 2B, LSB first
#define BITS (PREAMBLE_BITS + 16 + 8 * FASST_PACKET_LEN)
#define BARKER 0x712  // 11100010010 = bit 0, first chip in bit 10

// ID, plain (the TM-10's: on air 12 a8 d3 0d)
static uint8_t _id[4] = {FASST_ID_DEFAULT >> 24, FASST_ID_DEFAULT >> 16 & 0xFF,
                         FASST_ID_DEFAULT >> 8 & 0xFF, FASST_ID_DEFAULT & 0xFF};

void fasst_set_id(uint32_t id)
{
  for (int i = 0; i < 4; i++) _id[i] = id >> (24 - 8 * i);
}
// Whitening of the whole packet: a 16-bit LFSR, s[i] = s[i-4] ^ s[i-9] ^
// s[i-15] ^ s[i-16], bits LSB first, from a fixed seed (doc/futaba.md
// "Whitening, hop step")
static const uint8_t _white[FASST_PACKET_LEN] = {0x00, 0xC0, 0x4C, 0xED, 0xDE, 0x9B, 0xFF, 0xC6,
                                                 0x11, 0x77, 0x70, 0x1B, 0x0F, 0x08, 0x98, 0x65,
                                                 0x09, 0x95, 0x4E, 0x26, 0x57, 0xFE};

// F/S cycle: type bytes 12 / 19 as on air and the channels whose F/S
// position the packet carries in their slot (-1: none); bit 2 of byte 12
// (ch1..4, ch9, ch10) or 19 (ch5..8) on air = that channel holds instead.
// The other slots live
typedef struct {
  uint8_t b12, b19;
  int8_t fs[2];
} fs_type_t;

static const fs_type_t _fs_types[FASST_FS_TYPES] = {
    {0x8B, 0x82, {0, 4}}, {0x8A, 0x83, {1, 5}}, {0x81, 0x80, {2, 6}},
    {0x99, 0x90, {3, 7}}, {0x8B, 0x99, {8, -1}}, {0x8A, 0x99, {9, -1}},
};

void fasst_phy(radio_phy_t* phy, uint16_t tx_power)
{
  *phy = (radio_phy_t){
      .symbol_rate = 1500000,
      .deviation = 500000,
      .rx_bw = 100,  // RX unused
      .preamble_bytes = 1,
      .sync_bits = 32,
      .if_freq = (int16_t)0x8000,
      .tx_power = tx_power,
      .fsk = true,
  };
}

// k + step mod 37, skipping 36: CHQ 2349 + 2k, f = CHQ x 1.024 MHz
uint8_t fasst_next_hop(uint8_t k, uint8_t step)
{
  k = (k + step) % 37;
  return k == 36 ? (k + step) % 37 : k;
}

uint32_t fasst_freq_khz(uint8_t k) { return (2349 + 2 * k) * 1024; }

static uint16_t _crc_kermit(const uint8_t* p, uint32_t n)
{
  uint16_t c = 0;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ 0x8408 : c >> 1;
  }
  return c;
}

void fasst_packet(uint8_t* p, uint8_t k, uint8_t step, const uint16_t* ch, bool alt, uint8_t type,
                  const uint16_t* fs, uint16_t hold)
{
  // Slot values: the F/S position for the type's channels that don't hold
  uint16_t v[FASST_CHANNELS];
  memcpy(v, ch, sizeof(v));
  uint8_t b12 = 0x90, b19 = 0x99;
  bool fs_a = false, fs_b = false;  // group 1 slots taken by the type
  if (type) {
    const fs_type_t* t = &_fs_types[type - 1];
    b12 = t->b12;
    b19 = t->b19;
    for (int i = 0; i < 2; i++) {
      int c = t->fs[i];
      if (c < 0) continue;
      if (hold >> c & 1) {
        if (c >= 4 && c < 8) b19 |= 0x04;
        else b12 |= 0x04;
      } else {
        v[c] = fs[c];
      }
      if (c == 0 || c == 8) fs_a = true;
      if (c == 1 || c == 9) fs_b = true;
    }
  }
  // Group 1: slot a = ch1 or ch9 (flag), slot b = ch2 or ch10
  bool a9 = fs_a ? type == 5 : alt;
  bool b10 = fs_b ? type == 6 : alt;
  uint16_t g[4][2] = {{v[a9 ? 8 : 0], v[b10 ? 9 : 1]}, {v[2], v[3]}, {v[4], v[5]}, {v[6], v[7]}};
  uint8_t flags[4][2] = {{a9, b10}};

  // Plain packet: ID, hop position k + 1 and step, the groups, the type
  // bytes; CRC over bytes 0..19, then the whole packet whitened
  memcpy(p, _id, 4);
  p[4] = (k + 1) >> 1;
  p[5] = ((k + 1) & 1) << 7 | (step & 0x7F);
  static const uint8_t pos[4] = {6, 9, 13, 16};
  for (int i = 0; i < 4; i++) {
    uint32_t w = (g[i][0] & 0x7FF) | (uint32_t)flags[i][0] << 11 | (uint32_t)(g[i][1] & 0x7FF) << 12 |
                 (uint32_t)flags[i][1] << 23;
    for (int j = 0; j < 3; j++) p[pos[i] + j] = (uint8_t)(w >> (8 * j));
  }
  p[12] = b12 ^ _white[12];
  p[19] = b19 ^ _white[19];
  uint16_t crc = _crc_kermit(p, 20);
  p[20] = crc & 0xFF;
  p[21] = crc >> 8;
  for (int i = 0; i < FASST_PACKET_LEN; i++) p[i] ^= _white[i];
}

void fasst_spread(uint8_t* chips, const uint8_t* p)
{
  memset(chips, 0, FASST_CHIPS_LEN);
  uint32_t n = 0;
  for (uint32_t i = 0; i < BITS; i++) {
    uint32_t d;
    if (i < PREAMBLE_BITS) d = ~i & 1;  // 1010...10, a 0 before the sync
    else if (i < PREAMBLE_BITS + 16) d = SYNC >> (i - PREAMBLE_BITS) & 1;
    else {
      uint32_t j = i - PREAMBLE_BITS - 16;
      d = p[j / 8] >> (j % 8) & 1;
    }
    uint32_t code = d ? ~BARKER : BARKER;
    for (int c = 10; c >= 0; c--, n++)
      if (code >> c & 1) chips[n / 8] |= 0x80 >> (n % 8);
  }
}

uint32_t fasst_sync_word(const uint8_t* chips)
{
  return (uint32_t)chips[0] << 24 | (uint32_t)chips[1] << 16 | (uint32_t)chips[2] << 8 | chips[3];
}
