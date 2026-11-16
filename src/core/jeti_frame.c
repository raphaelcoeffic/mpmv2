// Jeti Duplex framing (jeti_frame.h)
#include "jeti_frame.h"

#include "platform.h"

// The 16 symbols' chip sequences in MSK form at 2 Mchip/s, first chip in bit
// 31; that bit depends on the previous symbol's last chip: left out. MSK bit
// k = chip k ^ chip k-1 ^ (k & 1), so bit 31 = first chip ^ previous last
static const uint32_t _msk[16] = {
    0x6077AE6C, 0x4E077AE6, 0x6CE077AE, 0x66CE077A, 0x2E6CE077, 0x7AE6CE07,
    0x77AE6CE0, 0x077AE6CE, 0x1F885193, 0x31F88519, 0x131F8851, 0x1931F885,
    0x51931F88, 0x051931F8, 0x0851931F, 0x78851931,
};
#define MSK_MASK 0x7FFFFFFF
// Bit s: the first / last chip of symbol s
#define FIRST_CHIPS 0xC3C3
#define LAST_CHIPS 0x27D8

void jeti_phy(radio_phy_t* phy, uint16_t tx_power)
{
  *phy = (radio_phy_t){
      .symbol_rate = 2000000,
      .deviation = 500000,
      .rx_bw = 100,
      .preamble_bytes = 4,
      .sync_bits = 32,
      .if_freq = (int16_t)0x8000,
      .tx_power = tx_power,
      .ble_2m = RADIO_2M_BLE,  // CC2652: 2 Mbps only there
  };
}

void jeti_header(uint8_t* f, uint8_t seq, uint16_t pan, uint16_t dst)
{
  f[0] = 0x01;
  f[1] = 0x08;
  f[2] = seq;
  f[3] = pan;
  f[4] = pan >> 8;
  f[5] = dst;
  f[6] = dst >> 8;
}

uint16_t jeti_crc16(const uint8_t* p, uint8_t n)
{
  uint16_t c = 0;
  while (n--) {
    c ^= *p++;
    for (int b = 0; b < 8; b++) c = c & 1 ? (c >> 1) ^ 0x8408 : c >> 1;
  }
  return c;
}

bool jeti_fcs_ok(const uint8_t* psdu, uint8_t len)
{
  return len >= JETI_FCS &&
         jeti_crc16(psdu, len - JETI_FCS) == (psdu[len - 2] | psdu[len - 1] << 8);
}

static uint8_t _key(uint8_t k, uint8_t p)
{
  return ((uint8_t)((k << 1) + p) ^ (k & 0x80 ? 0xC8 : 0)) ^ 0x93;
}

void jeti_scramble(const uint8_t* plain, uint8_t* out, uint8_t n)
{
  uint8_t k = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t p = plain[i];
    out[i] = i <= JETI_TYPE ? p : p ^ k;
    k = _key(k, p);
  }
}

void jeti_descramble(const uint8_t* in, uint8_t* out, uint8_t n)
{
  uint8_t k = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t p = i <= JETI_TYPE ? in[i] : in[i] ^ k;
    out[i] = p;
    k = _key(k, p);
  }
}

static uint8_t _put(uint8_t* air, uint8_t s, uint8_t prev_last)
{
  uint32_t w = _msk[s] | (uint32_t)(((FIRST_CHIPS >> s) ^ prev_last) & 1) << 31;
  air[0] = w >> 24;
  air[1] = w >> 16;
  air[2] = w >> 8;
  air[3] = w;
  return LAST_CHIPS >> s & 1;
}

static uint8_t _put_byte(uint8_t* air, uint8_t b, uint8_t last)
{
  last = _put(air, b & 0xF, last);
  return _put(air + 4, b >> 4, last);
}

uint16_t jeti_spread(uint8_t* air, const uint8_t* psdu, uint8_t len)
{
  uint8_t* a = air;
  uint8_t last = LAST_CHIPS & 1;  // after the sync word: symbol 0
  for (int i = 1; i < JETI_PREAMBLE_SYMBOLS; i++, a += 4) last = _put(a, 0, last);
  last = _put_byte(a, 0xA7, last);
  a += 8;
  last = _put_byte(a, len + JETI_FCS, last);
  a += 8;
  for (uint8_t i = 0; i < len; i++, a += 8) last = _put_byte(a, psdu[i], last);
  uint16_t fcs = jeti_crc16(psdu, len);
  last = _put_byte(a, fcs, last);
  a += 8;
  _put_byte(a, fcs >> 8, last);
  a += 8;
  // Trailing bits, alternating from the last one
  uint8_t t = a[-1] & 1 ? 0x55 : 0xAA;
  *a++ = t;
  *a++ = t;
  return a - air;
}

static uint8_t _symbol(const uint8_t* d, uint8_t* dist)
{
  uint32_t w = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
  uint8_t best = 0, bd = 32;
  for (uint8_t s = 0; s < 16; s++) {
    uint8_t n = __builtin_popcount((w ^ _msk[s]) & MSK_MASK);
    if (n < bd) {
      bd = n;
      best = s;
    }
  }
  *dist = bd;
  return best;
}

int jeti_despread(const uint8_t* d, uint16_t n, uint8_t* psdu, uint8_t* len, uint8_t* pre,
                  uint8_t* dmax)
{
  uint16_t syms = n / 4, i = 0;
  uint8_t dist, s = 0;
  *dmax = 0;
  while (i < syms && i < JETI_PREAMBLE_SYMBOLS && (s = _symbol(d + 4 * i, &dist)) == 0) i++;
  *pre = i;
  if (i + 4 > syms || s != 7 || _symbol(d + 4 * (i + 1), &dist) != 0xA) return -1;
  i += 2;
  uint8_t d0, d1;
  uint8_t lo = _symbol(d + 4 * i, &d0), hi = _symbol(d + 4 * (i + 1), &d1);
  *len = (lo | hi << 4) & 0x7F;
  *dmax = d0 > d1 ? d0 : d1;
  i += 2;
  int k = 0;
  for (; k < *len && i + 2 <= syms; k++, i += 2) {
    lo = _symbol(d + 4 * i, &d0);
    hi = _symbol(d + 4 * (i + 1), &d1);
    psdu[k] = lo | hi << 4;
    if (d0 > *dmax) *dmax = d0;
    if (d1 > *dmax) *dmax = d1;
  }
  return k;
}
