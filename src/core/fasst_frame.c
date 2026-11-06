// Futaba FASST 10ch framing (fasst_frame.h)
#include "fasst_frame.h"

#include <string.h>

#define PREAMBLE_BITS 64
#define SYNC 0x2BB1  // bit 0 first: B1 2B, LSB first
#define BITS (PREAMBLE_BITS + 16 + 8 * FASST_PACKET_LEN)
#define BARKER 0x712  // 11100010010 = bit 0, first chip in bit 10
#define BARKER_7 0x72  // 1110010, FASSTest 12CH

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

// FASSTest channel c: (2347 + c) x 1.024 MHz (FASST hop k = channel 2 + 2k)
uint32_t fasstest_freq_khz(uint8_t c) { return (2347 + c) * 1024; }

// Hop codewords, set A then set B (the R7206SB's table at 0xFFFC0E92)
static const uint8_t _hop_codes[2][23] = {
    {7, 9, 14, 18, 21, 27, 28, 35, 36, 42, 45, 49, 54, 56, 63, 65, 70, 72, 79, 83, 84, 90, 93},
    {4, 10, 13, 17, 22, 24, 31, 32, 39, 41, 46, 50, 53, 59, 60, 66, 69, 75, 76, 80, 87, 89, 94},
};

bool fasstest_hop_code(const uint8_t* p, uint8_t* set, uint8_t* row)
{
  uint8_t code = (p[1] & 0x0F) << 3 | p[0] >> 5;
  for (int s = 0; s < 2; s++)
    for (int r = 0; r < 23; r++)
      if (_hop_codes[s][r] == code) {
        *set = s;
        *row = r;
        return true;
      }
  return false;
}

// Hop i of a row: set A 2 + 3 ((5^i + row) mod 23), set B 2 + 2 ((5^i - 1
// + row) mod 23)
bool fasstest_next_channel(uint8_t set, uint8_t row, uint8_t c, uint8_t* next)
{
  uint8_t ch[FASSTEST_HOPS];
  uint32_t p = 1;
  for (int i = 0; i < FASSTEST_HOPS; i++, p = p * 5 % 23)
    ch[i] = set ? 2 + 2 * ((p + 22 + row) % 23) : 2 + 3 * ((p + row) % 23);
  for (int i = 0; i < FASSTEST_HOPS; i++)
    if (ch[i] == c) {
      *next = ch[(i + 1) % FASSTEST_HOPS];
      return true;
    }
  return false;
}

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

bool fasst_unpack(const uint8_t* p, uint8_t* plain, uint8_t* k, uint8_t* step)
{
  for (int i = 0; i < FASST_PACKET_LEN; i++) plain[i] = p[i] ^ _white[i];
  if (_crc_kermit(plain, 20) != (plain[20] | plain[21] << 8)) return false;
  // Hop position k + 1, the step: fasst_packet()
  uint8_t pos = plain[4] << 1 | plain[5] >> 7;
  *k = pos - 1;
  *step = plain[5] & 0x7F;
  return pos >= 1 && *k < FASST_HOPS && *step >= 1 && *step < 37;
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

// Barker code of n chips (bit 0), first chip in bit n - 1
static uint32_t _barker(uint8_t n) { return n == FASST_BARKER_7 ? BARKER_7 : BARKER; }

uint32_t fasst_rx_sync_word(uint8_t n)
{
  // The chips of sync bits 3.., the ones past 32 dropped
  uint32_t code = _barker(n), mask = (1 << n) - 1;
  uint64_t w = 0;
  for (int i = 3; i < 3 + FASST_RX_SYNC_BITS(n); i++) w = w << n | (SYNC >> i & 1 ? ~code & mask : code);
  return (uint32_t)(w >> FASST_RX_SKIP_CHIPS(n));
}

uint16_t fasst_rx_sync_left(uint8_t n) { return SYNC >> (16 - FASST_RX_SYNC_LEFT(n)); }

void fasst_despread(const uint8_t* chips, uint8_t n, uint32_t first, uint32_t b, uint8_t* out,
                    uint8_t* corr)
{
  uint32_t code = _barker(n), mask = (1 << n) - 1;
  memset(out, 0, (b + 7) / 8);
  for (uint32_t i = 0, c = first; i < b; i++, c += n) {
    // n chips from chip c: within the 3 bytes from c / 8
    const uint8_t* p = chips + c / 8;
    uint32_t w = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    uint32_t x = (w >> (24 - n - c % 8) & mask) ^ code;
    int d = __builtin_popcount(x);  // chips off the Barker code (bit 0)
    if (2 * d > n) out[i / 8] |= 1 << (i % 8);
    corr[i] = 2 * d > n ? 2 * d - n : n - 2 * d;
  }
}
