#include "tfhss.h"

// Row 0: 31, then 3^i mod 31 (i = 0..29); row r: (row 0 + r - 1 mod 31) + 1
// (a Welch-Costas array, the R334SBS's table at 0xFFFE0261)
static const uint8_t _row0[TFHSS_CHANNELS] = {31, 1,  3,  9,  27, 19, 26, 16, 17, 20, 29,
                                              25, 13, 8,  24, 10, 30, 28, 22, 4,  12, 5,
                                              15, 14, 11, 2,  6,  18, 23, 7,  21};

// The other mode's list (0xFFFE0641): channels 1..25, low / high pairs
#define LIST_LEN 16
static const uint8_t _list[LIST_LEN] = {1, 25, 3, 22, 4, 21, 6, 19, 7, 18, 9, 16, 10, 15, 12, 13};

uint8_t tfhss_channel(uint8_t row, uint8_t pos)
{
  if (row == TFHSS_ROW_LIST) return _list[pos % LIST_LEN];
  return (_row0[pos % TFHSS_CHANNELS] + row % TFHSS_ROWS - 1) % TFHSS_CHANNELS + 1;
}

uint8_t tfhss_next(uint8_t row, uint8_t n)
{
  uint8_t len = row == TFHSS_ROW_LIST ? LIST_LEN : TFHSS_CHANNELS;
  for (uint8_t i = 0; i < len; i++)
    if (tfhss_channel(row, i) == n) return tfhss_channel(row, (i + 1) % len);
  return tfhss_channel(row, 0);
}

uint16_t tfhss_crc(const uint8_t* p, uint8_t n, uint16_t crc)
{
  while (n--) {
    crc ^= *p++;
    for (int i = 0; i < 8; i++) crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
  }
  return crc;
}

void tfhss_packet(uint8_t* out, const uint8_t* plain, const uint8_t* key)
{
  uint16_t crc = tfhss_crc(plain, TFHSS_DATA_LEN, 0xFFFF);
  for (int i = 0; i < TFHSS_DATA_LEN; i++) out[i] = plain[i] ^ key[i];
  out[12] = crc;
  out[13] = crc >> 8;
}

uint16_t tfhss_syndrome(const uint8_t* p)
{
  return tfhss_crc(p, TFHSS_DATA_LEN, 0xFFFF) ^ (p[12] | p[13] << 8);
}

uint16_t tfhss_key_syndrome(const uint8_t* p, uint8_t n)
{
  uint8_t ch[TFHSS_DATA_LEN] = {0};
  ch[7] = n;
  return tfhss_syndrome(p) ^ tfhss_crc(ch, TFHSS_DATA_LEN, 0);
}
