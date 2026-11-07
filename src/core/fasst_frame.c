// Futaba FASST 10ch framing (fasst_frame.h)
#include "fasst_frame.h"

#include <string.h>

#define PREAMBLE_BITS 64
#define SYNC 0x2BB1  // bit 0 first: B1 2B, LSB first
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

// k + step mod 37, skipping 36 (France: k + s mod 23, skipping 0): CHQ
// 2349 + 2k, f = CHQ x 1.024 MHz
uint8_t fasst_next_hop(uint8_t k, uint8_t step)
{
  if (step > FASST_FRANCE_STEP) {
    step -= FASST_FRANCE_STEP;
    k = (k + step) % 23;
    return k == 0 ? step : k;
  }
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
uint8_t fasstest_channel(uint8_t set, uint8_t row, uint8_t i)
{
  uint32_t p = 1;
  while (i--) p = p * 5 % 23;
  return set ? 2 + 2 * ((p + 22 + row) % 23) : 2 + 3 * ((p + row) % 23);
}

uint8_t fasstest_code(uint8_t set, uint8_t row) { return _hop_codes[set][row]; }

bool fasstest_next_channel(uint8_t set, uint8_t row, uint8_t c, uint8_t* next)
{
  for (int i = 0; i < FASSTEST_HOPS; i++)
    if (fasstest_channel(set, row, i) == c) {
      *next = fasstest_channel(set, row, (i + 1) % FASSTEST_HOPS);
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

// Plain packet: ID, hop position k + 1 and step, the groups (2 slots of
// 11 bits + flag each), the type bytes 12 / 19 (plain); CRC over bytes
// 0..19, then the whole packet whitened
static void _fasst_pack(uint8_t* p, uint8_t k, uint8_t step, const uint16_t g[4][2],
                        const uint8_t flags[4][2], uint8_t b12, uint8_t b19)
{
  memcpy(p, _id, 4);
  p[4] = (k + 1) >> 1;
  p[5] = ((k + 1) & 1) << 7 | (step & 0x7F);
  static const uint8_t pos[4] = {6, 9, 13, 16};
  for (int i = 0; i < 4; i++) {
    uint32_t w = (g[i][0] & 0x7FF) | (uint32_t)flags[i][0] << 11 | (uint32_t)(g[i][1] & 0x7FF) << 12 |
                 (uint32_t)flags[i][1] << 23;
    for (int j = 0; j < 3; j++) p[pos[i] + j] = (uint8_t)(w >> (8 * j));
  }
  p[12] = b12;
  p[19] = b19;
  uint16_t crc = _crc_kermit(p, 20);
  p[20] = crc & 0xFF;
  p[21] = crc >> 8;
  for (int i = 0; i < FASST_PACKET_LEN; i++) p[i] ^= _white[i];
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
  _fasst_pack(p, k, step, g, flags, b12 ^ _white[12], b19 ^ _white[19]);
}

// FASST MULT F/S packets: the slot pair of each type's code, (group,
// slot) = (q >> 1, q & 1) and (q >> 1) + 2
static const uint8_t _mult_fs_codes[4] = {0x00, 0x01, 0x02, 0x12};

void fasst_mult_packet(uint8_t* p, uint8_t k, uint8_t step, const uint16_t* ch, bool alt,
                       uint8_t type, const uint16_t* fs, uint16_t hold)
{
  uint16_t g[4][2];
  uint8_t flags[4][2];
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 2; j++) {
      flags[i][j] = alt;
      g[i][j] = ch[2 * i + j + (alt ? 8 : 0)];
    }
  }
  uint8_t m[2] = {FASST_MULT_NORMAL, FASST_MULT_NORMAL};
  if (type) {
    uint8_t q = (type - 1) & 3;
    bool high = type <= 4;
    for (int i = 0; i < 2; i++) {
      uint8_t grp = (q >> 1) + 2 * i, slot = q & 1;
      uint8_t c = 2 * grp + slot + (high ? 8 : 0);
      bool h = hold >> c & 1;
      flags[grp][slot] = high;
      g[grp][slot] = h ? ch[c] : fs[c];
      m[i] = _mult_fs_codes[q] | (h ? 0 : 1) << 2;
    }
  }
  _fasst_pack(p, k, step, g, flags, 0x80 | m[0], 0xA0 | m[1]);
}

// FASST 7CH whitening: the 10ch stream from offset 46893
static const uint8_t _white7[FASST7_PACKET_LEN] = {0x00, 0x80, 0x88, 0x49, 0x6B, 0xED,
                                                   0xAA, 0x84, 0x1E, 0x5A, 0xA0, 0xED,
                                                   0xF5, 0x0F, 0x10, 0xB9, 0xF1};

void fasst7_packet(uint8_t* p, uint8_t k, uint8_t step, const uint16_t* ch, bool fs_packet,
                   int16_t fs3)
{
  memset(p, 0, FASST7_PACKET_LEN);
  memcpy(p, _id, 3);
  p[3] = (_id[3] & 0xE0) | (k + 1) >> 1;
  p[4] = ((k + 1) & 1) << 7;
  // Bits from byte 4 bit 5 on, MSB first
  uint8_t code = fs_packet ? (fs3 < 0 ? 3 : 2) : (fs3 < 0 ? 0 : 1);
  uint32_t pos = 4 * 8 + 2;  // bit index from byte 0 bit 7
  for (int i = 0; i <= FASST7_CHANNELS; i++) {
    uint16_t v = i == FASST7_CHANNELS ? (step & 0x3F) << 3 | code
                 : fs_packet && i == 2 && fs3 >= 0 ? fs3 : ch[i];
    uint8_t n = i == FASST7_CHANNELS ? 9 : 11;
    for (int b = n - 1; b >= 0; b--, pos++)
      if (v >> b & 1) p[pos / 8] |= 0x80 >> (pos % 8);
  }
  uint16_t crc = _crc_kermit(p, 15);
  p[15] = crc & 0xFF;
  p[16] = crc >> 8;
  for (int i = 0; i < FASST7_PACKET_LEN; i++) p[i] ^= _white7[i];
}

bool fasst7_unpack(const uint8_t* p, uint8_t* plain, uint8_t* k, uint8_t* step, uint16_t* ch,
                   uint8_t* code)
{
  for (int i = 0; i < FASST7_PACKET_LEN; i++) plain[i] = p[i] ^ _white7[i];
  if (_crc_kermit(plain, 15) != (plain[15] | plain[16] << 8)) return false;
  uint8_t hop = (plain[3] & 0x1F) << 1 | plain[4] >> 7;
  uint32_t pos = 4 * 8 + 2;
  uint16_t v[FASST7_CHANNELS + 1];
  for (int i = 0; i <= FASST7_CHANNELS; i++) {
    uint8_t n = i == FASST7_CHANNELS ? 9 : 11;
    v[i] = 0;
    for (int b = 0; b < n; b++, pos++) v[i] = v[i] << 1 | (plain[pos / 8] >> (7 - pos % 8) & 1);
  }
  if (ch) memcpy(ch, v, FASST7_CHANNELS * sizeof(*v));
  *k = hop - 1;
  *step = v[FASST7_CHANNELS] >> 3;
  if (code) *code = v[FASST7_CHANNELS] & 3;
  if (*step > FASST_FRANCE_STEP)
    return *step <= FASST_FRANCE_STEP + FASST_FRANCE_HOPS && *k >= 1 && *k <= FASST_FRANCE_HOPS;
  return hop >= 1 && *k < FASST_HOPS && *step >= 1 && *step < 37;
}

// Plain bytes of a FASSTest packet before its CRC
static uint8_t _fasstest_len(bool ch12)
{
  return ch12 ? FASSTEST_PACKET_LEN_12CH - 2 : FASSTEST_PACKET_LEN - 2;
}

// Bytes 2..len - 1 whitened with 00 00 ID0..5 c ID0..5 c ID0 ID1, id0 = ID0..3
static void _fasstest_whiten(uint8_t* p, uint8_t len, const uint8_t* id0, const uint8_t* id,
                             uint8_t c)
{
  const uint8_t mask[18] = {0,      0,      id0[0], id0[1], id0[2], id0[3], id[4],  id[5],  c,
                            id0[0], id0[1], id0[2], id0[3], id[4],  id[5],  c,      id0[0], id0[1]};
  for (int i = 2; i < len; i++) p[i] ^= mask[i];
}

static void _fasstest_crc(uint8_t* p, uint8_t len)
{
  uint16_t crc = _crc_kermit(p, len);
  p[len] = crc & 0xFF;
  p[len + 1] = crc >> 8;
}

// Data packet: the slots' values v (8, 12CH 5) with flag f, mode field m
// (6 bits: b1 bits 6..7, b2 bits 0..3)
static void _fasstest_data(uint8_t* p, bool ch12, const uint8_t* id, uint8_t code, uint8_t c,
                           const uint16_t* v, bool f, uint8_t m, uint8_t req)
{
  // Slot i: 11 bits + flag (ch i + 1 or i + 9; 12CH i + 6), the slots in
  // 3-byte pairs from byte 2 bit 4: even slots low nibble first, odd ones
  // low byte first
  uint8_t len = _fasstest_len(ch12), slots = ch12 ? 5 : 8;
  memset(p, 0, len + 2);
  p[0] = (ch12 ? 3 : 0) | (code & 7) << 5;
  p[1] = (m & 3) << 6 | code >> 3;
  p[2] = m >> 2 & 0x0F;
  for (int i = 0; i < slots; i += 2) {
    uint8_t* q = p + 2 + 3 * (i / 2);
    uint16_t a = v[i] & 0x7FF;
    q[0] |= (a & 0x0F) << 4;
    q[1] = a >> 4 | f << 7;
    if (i + 1 < slots) {
      uint16_t b = v[i + 1] & 0x7FF;
      q[2] = b;
      q[3] = b >> 8 | f << 3;
    }
  }
  if (req) {
    p[1] |= 0x20;
    if (!ch12) p[16] = 1 | (req - 1) << 1;
  }
  _fasstest_crc(p, len);
  _fasstest_whiten(p, len, id, id, c);
}

void fasstest_packet(uint8_t* p, bool ch12, const uint8_t* id, uint8_t code, uint8_t c,
                     const uint16_t* ch, bool high, uint8_t req)
{
  _fasstest_data(p, ch12, id, code, c, ch + (high ? (ch12 ? 5 : 8) : 0), high, 0x13, req);
}

void fasstest_fs_packet(uint8_t* p, bool ch12, const uint8_t* id, uint8_t code, uint8_t c,
                        const uint16_t* ch, uint8_t n, uint8_t mode, uint16_t value)
{
  static const uint8_t slot_code[4] = {0x00, 0x01, 0x02, 0x12};
  uint8_t slots = ch12 ? 5 : 8, half = n / slots, s = n % slots;
  uint16_t v[8];
  memcpy(v, ch + half * slots, slots * sizeof(v[0]));
  v[s] = value;
  uint8_t m = slot_code[s % 4] | (s >= 4) << 5 | (mode & 3) << 2;
  _fasstest_data(p, ch12, id, code, c, v, half, m, 0);
}

bool fasstest_reply_unpack(const uint8_t* p, bool ch12, uint8_t* plain, const uint8_t* rx,
                           const uint8_t* id, uint8_t c)
{
  uint8_t len = _fasstest_len(ch12);
  memcpy(plain, p, len);
  _fasstest_whiten(plain, len, rx, id, c);
  return _crc_kermit(plain, len) == (p[len] | p[len + 1] << 8);
}

void fasstest_reply_packet(uint8_t* p, bool ch12, uint8_t code, uint8_t page, const uint8_t* data,
                           const uint8_t* rx, const uint8_t* id, uint8_t c)
{
  uint8_t len = _fasstest_len(ch12);
  p[0] = FASSTEST_TYPE_REPLY | (ch12 ? 3 : 0) | (code & 7) << 5;
  p[1] = (ch12 ? 0 : (page & 3) << 4) | code >> 3;
  memcpy(p + 2, data, len - 2);
  _fasstest_crc(p, len);
  _fasstest_whiten(p, len, rx, id, c);
}

// x (16 bits) with sum of bit i x col[i] = t over GF(2), false if none
static bool _solve16(const uint16_t* col, uint16_t t, uint16_t* x)
{
  // Pivots: a combination of columns (mask) and its value, by highest bit
  uint16_t pv[16], pm[16], pb[16];
  int n = 0;
  for (int i = 0; i < 16; i++) {
    uint16_t v = col[i], m = 1 << i;
    for (int j = 0; j < n; j++)
      if (v & pb[j]) {
        v ^= pv[j];
        m ^= pm[j];
      }
    if (!v) continue;
    pb[n] = 0x8000 >> __builtin_clz((uint32_t)v << 16);
    pv[n] = v;
    pm[n++] = m;
  }
  *x = 0;
  for (int j = 0; j < n; j++)
    if (t & pb[j]) {
      t ^= pv[j];
      *x ^= pm[j];
    }
  return t == 0;
}

bool fasstest_reply_serial(const uint8_t* p, bool ch12, const uint8_t* id, uint8_t c, uint8_t* rx)
{
  uint8_t plain[FASSTEST_PACKET_LEN];
  if (!ch12) {
    // The serial's second copy (bytes 9..12), or its first (2..5)
    static const uint8_t at[2] = {9, 2};
    for (int i = 0; i < 2; i++) {
      memcpy(rx, p + at[i], 4);
      if (fasstest_reply_unpack(p, false, plain, rx, id, c)) return true;
    }
    return false;
  }
  // Bytes 5..9 plain 0: ID3, ID4, ID5, c, ID0 on air
  if (p[6] != id[4] || p[7] != id[5] || p[8] != c) return false;
  rx[0] = p[9];
  rx[3] = p[5];
  // The CRC is linear (no init, no final XOR): the plain bytes 3..4 (rx[1],
  // rx[2] on air) from the CRC of the rest
  uint8_t len = _fasstest_len(true), z[FASSTEST_PACKET_LEN_12CH] = {0};
  uint16_t col[16];
  for (int i = 0; i < 16; i++) {
    z[3 + i / 8] = 1 << (i % 8);
    col[i] = _crc_kermit(z, len);
    z[3 + i / 8] = 0;
  }
  rx[1] = rx[2] = 0;
  memcpy(plain, p, len);
  _fasstest_whiten(plain, len, rx, id, c);
  plain[3] = plain[4] = 0;
  uint16_t x;
  if (!_solve16(col, _crc_kermit(plain, len) ^ (p[len] | p[len + 1] << 8), &x)) return false;
  rx[1] = p[3] ^ (x & 0xFF);
  rx[2] = p[4] ^ (x >> 8);
  return true;
}

void fasstest_link_packet(uint8_t* p, bool ch12, const uint8_t* id, uint8_t code, uint8_t c,
                          uint8_t bfs)
{
  memset(p, 0, FASSTEST_PACKET_LEN);
  p[0] = 0x08 | (code & 7) << 5;
  p[1] = 0x30 | code >> 3;  // bits 4..5 = 3 (meaning unknown), receiver 0
  memcpy(p + 2, id, FASSTEST_ID_LEN);
  p[8] = bfs;
  p[9] = c;
  p[14] = ch12 ? 0x02 : 0;
  p[15] = 0x02;  // the R7206SB: low-voltage time
  _fasstest_crc(p, FASSTEST_PACKET_LEN - 2);
}

bool fasstest_link_check(const uint8_t* p, uint8_t c)
{
  uint8_t len = FASSTEST_PACKET_LEN - 2;
  return (p[0] & 0x1C) == 0x08 && p[9] == c &&
         _crc_kermit(p, len) == (p[len] | p[len + 1] << 8);
}

void fasstest_link_reply(uint8_t* p, const uint8_t* link, const uint8_t* rx)
{
  memset(p, 0, FASSTEST_PACKET_LEN);
  p[0] = 0xF8;  // as the R7008SB's
  memcpy(p + 2, link + 2, FASSTEST_ID_LEN + 2);  // ID, battery F/S, channel
  memcpy(p + 10, rx, 4);
  p[14] = link[14];
  p[15] = link[15];
  _fasstest_crc(p, FASSTEST_PACKET_LEN - 2);
}

bool fasst_unpack(const uint8_t* p, uint8_t* plain, uint8_t* k, uint8_t* step)
{
  for (int i = 0; i < FASST_PACKET_LEN; i++) plain[i] = p[i] ^ _white[i];
  if (_crc_kermit(plain, 20) != (plain[20] | plain[21] << 8)) return false;
  // Hop position k + 1, the step: fasst_packet()
  uint8_t pos = plain[4] << 1 | plain[5] >> 7;
  *k = pos - 1;
  *step = plain[5] & 0x7F;
  if (*step > FASST_FRANCE_STEP)
    return *step <= FASST_FRANCE_STEP + FASST_FRANCE_HOPS && *k >= 1 && *k <= FASST_FRANCE_HOPS;
  return pos >= 1 && *k < FASST_HOPS && *step >= 1 && *step < 37;
}

void fasst_spread(uint8_t* chips, const uint8_t* p) { fasst_spread_len(chips, p, FASST_PACKET_LEN); }

void fasst_spread_len(uint8_t* chips, const uint8_t* p, uint32_t len)
{
  fasst_spread_n(chips, FASST_BARKER_11, p, len);
}

// Barker code of n chips (bit 0), first chip in bit n - 1
static uint32_t _barker(uint8_t n) { return n == FASST_BARKER_7 ? BARKER_7 : BARKER; }

void fasst_spread_n(uint8_t* chips, uint8_t n, const uint8_t* p, uint32_t len)
{
  uint32_t bits = PREAMBLE_BITS + 16 + 8 * len, barker = _barker(n), mask = (1 << n) - 1;
  memset(chips, 0, (bits * n + 7) / 8);
  uint32_t k = 0;
  for (uint32_t i = 0; i < bits; i++) {
    uint32_t d;
    if (i < PREAMBLE_BITS) d = ~i & 1;  // 1010...10, a 0 before the sync
    else if (i < PREAMBLE_BITS + 16) d = SYNC >> (i - PREAMBLE_BITS) & 1;
    else {
      uint32_t j = i - PREAMBLE_BITS - 16;
      d = p[j / 8] >> (j % 8) & 1;
    }
    uint32_t code = (d ? ~barker : barker) & mask;
    for (int c = n - 1; c >= 0; c--, k++)
      if (code >> c & 1) chips[k / 8] |= 0x80 >> (k % 8);
  }
}

uint32_t fasst_sync_word(const uint8_t* chips)
{
  return (uint32_t)chips[0] << 24 | (uint32_t)chips[1] << 16 | (uint32_t)chips[2] << 8 | chips[3];
}

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

static uint8_t _rx_bits[(FASST_RX_BITS_MAX + 7) / 8 + 1], _rx_corr[FASST_RX_BITS_MAX];

// 8 bits from bit b, LSB first
static uint8_t _rx_byte(uint32_t b)
{
  return _rx_bits[b / 8] >> (b % 8) | _rx_bits[b / 8 + 1] << (8 - b % 8);
}

int32_t fasst_rx_bytes(const uint8_t* data, uint8_t n, uint16_t bytes, uint8_t* out, uint8_t* q)
{
  fasst_despread(data, n, FASST_RX_SKIP_CHIPS(n), FASST_RX_BITS(n, bytes), _rx_bits, _rx_corr);
  uint16_t left = fasst_rx_sync_left(n), mask = (1 << FASST_RX_SYNC_LEFT(n)) - 1;
  uint32_t shift = 0;
  while (shift <= FASST_RX_EARLY_BITS &&
         ((_rx_byte(shift) | (uint16_t)_rx_byte(shift + 8) << 8) & mask) != left)
    shift++;
  if (shift > FASST_RX_EARLY_BITS) return -1;

  uint32_t b0 = shift + FASST_RX_SYNC_LEFT(n);
  for (uint32_t i = 0; i < bytes; i++) {
    out[i] = _rx_byte(b0 + 8 * i);
    if (!q) continue;
    q[i] = n;
    for (int j = 0; j < 8; j++)
      if (_rx_corr[b0 + 8 * i + j] < q[i]) q[i] = _rx_corr[b0 + 8 * i + j];
  }
  return shift;
}
