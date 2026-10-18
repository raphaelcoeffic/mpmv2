#include <string.h>

#include "frame.h"

// Frame buffer: header + prefix + longest encoded payload + CRC
#define FRAME_BUF_LEN (FRAME_HDR_MAX + FRAME_PREFIX_MAX + FRAME_PAYLOAD_MAX + 8)

#define NRF_CRC16_INIT 0xFFFF
#define NRF_CRC8_INIT 0xFF
#define XN297_CRC_INIT 0xB5D2
#define CC2500_CRC_INIT 0xFFFF
#define A7105_CRC_INIT 0x1D0F  // CRC-16 CCITT, over the payload (measured)

// CYRF6936 (measured), as CC2652 bits: preamble (1100 repeated, 64 chips), the
// SOP code (bytes sent LSB first), 4 alternating chips (0101 or 1010: the last
// chip inverted first), the SOP code again (inverted with 64 chip data codes), a
// gap chip (the last chip inverted), then one 8DR symbol per
// byte: the data code rotated left by bits 0-5, the second data code if bit 6,
// inverted unless bit 7. 32 chip data codes: 4 of them (bits 5-6), rotated by
// bits 0-4. 32 chip SOP: the first 4 code bytes. SDR (DSM bind, no SOP): one 64
// chip symbol per bit, LSB first: the data code for 1, inverted for 0.
// CRC-16/ARC (0x8005 reflected, xorout 0xFFFF) over the length and payload,
// from the bit reversed seed, sent low byte first.
// TX_OVERRIDE.TX_INV inverts all of it. _put_chips() / _get_chips(): CC2652
// bit = !chip (TX_INV: chip).
#define CYRF_SDR_CHIPS 64  // SDR: 64 chip codes only
#define CYRF_GAP_CHIPS 1
#define CYRF_SYNC_CHIPS 23  // chips of the first symbol in the RX sync word (no SOP)
#define CYRF_PREAMBLE_NIBBLES 17  // 68 chips: header on whole bytes (with SOP)

// A7105 FEC (measured): each nibble, high one first, as a 7 bit Hamming code
// word, its data bits first
static const uint8_t _a7105_fec[16] = {0x00, 0x0B, 0x16, 0x1D, 0x27, 0x2C, 0x31, 0x3A,
                                       0x45, 0x4E, 0x53, 0x58, 0x62, 0x69, 0x74, 0x7F};

// CC2500: last preamble bytes before the sync word (at least 2 are sent)
#define CC2500_PREAMBLE 0xAA
#define CC2500_PREAMBLE_LEN 2

// CC2500 FEC (TI DN504): rate 1/2 convolutional code (K=4, 2 bit symbol per
// data bit) and 4x4 interleaving of the symbols, by blocks of 2 data bytes. The
// data is padded to an even length with 1 or 2 trellis terminator bytes.
#define CC2500_FEC_TERM 0x0B
static const uint8_t _fec_table[16] = {0, 3, 1, 2, 3, 0, 2, 1, 3, 0, 2, 1, 0, 3, 1, 2};

// nRF24L01+ packet control field: 6 bit length, 2 bit pid, 1 bit "ack
// requested" (the datasheet's NO_ACK flag, sent inverted). Without dynamic
// payload length, the length field is 0x33 (measured).
#define ESB_PCF_BITS 9
#define ESB_STATIC_LEN 0x33

// XN297 preamble, sent before the address
static const uint8_t _xn297_preamble[] = {0x0C, 0x71, 0x0F, 0x55};

// XN297 tables, from the MULTI-Module XN297 emulation (XN297_EMU.ino)

// address / pcf / payload scramble table
static const uint8_t _xn297_scramble[] = {
    0xE3, 0xB1, 0x4B, 0xEA, 0x85, 0xBC, 0xE5, 0x66, 0x0D, 0xAE, 0x8C, 0x88, 0x12,
    0x69, 0xEE, 0x1F, 0xC7, 0x62, 0x97, 0xD5, 0x0B, 0x79, 0xCA, 0xCC, 0x1B, 0x5D,
    0x19, 0x10, 0x24, 0xD3, 0xDC, 0x3F, 0x8E, 0xC5, 0x2F, 0xAA, 0x16, 0xF3, 0x95};

// CRC xorout, indexed by address length - 3 + payload length
static const uint16_t _xn297_xorout_scrambled[] = {
    0x0000, 0x3448, 0x9BA7, 0x8BBB, 0x85E1, 0x3E8C, 0x451E, 0x18E6, 0x6B24,
    0xE7AB, 0x3828, 0x814B, 0xD461, 0xF494, 0x2503, 0x691D, 0xFE8B, 0x9BA7,
    0x8B17, 0x2920, 0x8B5F, 0x61B1, 0xD391, 0x7401, 0x2138, 0x129F, 0xB3A0,
    0x2988, 0x23CA, 0xC0CB, 0x0C6C, 0xB329, 0xA0A1, 0x0A16, 0xA9D0};

static const uint16_t _xn297_xorout[] = {
    0x0000, 0x3D5F, 0xA6F1, 0x3A23, 0xAA16, 0x1CAF, 0x62B2, 0xE0EB, 0x0821,
    0xBE07, 0x5F1A, 0xAF15, 0x4F0A, 0xAD24, 0x5E48, 0xED34, 0x068C, 0xF2C9,
    0x1852, 0xDF36, 0x129D, 0xB17C, 0xD5F5, 0x70D7, 0xB798, 0x5133, 0x67DB,
    0xD94E, 0x0A5B, 0xE445, 0xE6A5, 0x26E7, 0xBDAB, 0xC379, 0x8E20};

static const uint16_t _xn297_xorout_scrambled_enhanced[] = {
    0x0000, 0x7EBF, 0x3ECE, 0x07A4, 0xCA52, 0x343B, 0x53F8, 0x8CD0, 0x9EAC,
    0xD0C0, 0x150D, 0x5186, 0xD251, 0xA46F, 0x8435, 0xFA2E, 0x7EBD, 0x3C7D,
    0x94E0, 0x3D5F, 0xA685, 0x4E47, 0xF045, 0xB483, 0x7A1F, 0xDEA2, 0x9642,
    0xBF4B, 0x032F, 0x01D2, 0xDC86, 0x92A5, 0x183A, 0xB760, 0xA953};

static const uint16_t _xn297_xorout_enhanced[] = {
    0x0000, 0x8BE6, 0xD8EC, 0xB87A, 0x42DC, 0xAA89, 0x83AF, 0x10E4, 0xE83E,
    0x5C29, 0xAC76, 0x1C69, 0xA4B2, 0x5961, 0xB4D3, 0x2A50, 0xCB27, 0x5128,
    0x7CDB, 0x7A14, 0xD5D2, 0x57D7, 0xE31D, 0xCE42, 0x648D, 0xBF2D, 0x653B,
    0x190C, 0x9117, 0x9A97, 0xABFC, 0xE68E, 0x0DE7, 0x28A2, 0x1965};

#define XN297_XOROUT_LEN (sizeof(_xn297_xorout) / sizeof(_xn297_xorout[0]))

// CYRF6936 GFSK mode (1 Mbps, bytes sent LSB first): the MULTI-Module builds
// HS6200 and RF2500 packets in software. Its preamble is 0xAA on air
// (measured), in the frame so the CC2652 sends the same one.
#define GFSK1M_PREAMBLE 0xAA

// HS6200 (MULTI-Module HS6200_EMU.ino): the address (last byte first), 2 guard
// bytes (the last address byte again), a packet control field as ESB (6 bit
// length, 2 bit pid, no ack bit), the scrambled payload, CRC-16 CCITT from
// 0xFFFF over the address, the packet control field and the payload
#define HS6200_GUARD_LEN 2
static const uint8_t _hs6200_scramble[] = {0x80, 0xF5, 0x3B, 0x0D, 0x6D, 0x2A, 0xF9, 0xBC,
                                           0x51, 0x8E, 0x4C, 0xFD, 0xC1, 0x65, 0xD0};

// RF2500 (RF2500_EMU.ino): 4 address bytes, 2 sync bytes, then the payload and
// CRC, each bit (LSB first) sent as 4: 1100 for 1, 0011 for 0. CRC-16 CCITT
// from 0 over the bits as sent, MSB first. Scrambled: payload XORed with a
// table, CRC with a value per length (16 or 19 bytes, E129)
#define RF2500_ADDR_LEN 4
#define RF2500_SYNC 0xC3
#define RF2500_ONE 0xC
#define RF2500_ZERO 0x3
static const uint8_t _rf2500_scramble[] = {0xD0, 0x9E, 0x53, 0x33, 0xD8, 0xBA, 0x98,
                                           0x08, 0x24, 0xCB, 0x3B, 0xFC, 0x71, 0xA3,
                                           0xF4, 0x55, 0x68, 0x4F, 0xA9};
#define RF2500_XOROUT_16 0xAEE4
#define RF2500_XOROUT_19 0xE7C5

static uint8_t _bit_reverse(uint8_t b)
{
#if defined(__ARM_ARCH) && !defined(__aarch64__)
  uint32_t r;
  __asm__("rbit %0, %1" : "=r"(r) : "r"((uint32_t)b));
  return r >> 24;
#else  // host unit tests
  uint8_t r = 0;
  for (int i = 0; i < 8; i++, b >>= 1) r = (r << 1) | (b & 1);
  return r;
#endif
}

// CRC-16 CCITT, MSB first, on the first 'bits' bits of b
static uint16_t _crc16(uint16_t crc, uint8_t b, uint8_t bits)
{
  crc ^= (uint16_t)b << 8;
  while (bits--) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
  return crc;
}

static uint16_t _crc16_buf(uint16_t crc, const uint8_t* data, uint8_t len)
{
  while (len--) crc = _crc16(crc, *data++, 8);
  return crc;
}

// CC2500 CRC-16: polynomial 0x8005, MSB first
static uint16_t _cc2500_crc_buf(uint16_t crc, const uint8_t* data, uint8_t len)
{
  while (len--) {
    crc ^= (uint16_t)*data++ << 8;
    for (int i = 0; i < 8; i++) crc = crc & 0x8000 ? (crc << 1) ^ 0x8005 : crc << 1;
  }
  return crc;
}

static uint8_t _crc8_buf(uint8_t crc, const uint8_t* data, uint8_t len)
{
  while (len--) {
    crc ^= *data++;
    for (int i = 0; i < 8; i++) crc = crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1;
  }
  return crc;
}

// Bit fields, MSB first
static void _put_bits(uint8_t* buf, uint16_t* pos, uint32_t v, uint8_t bits)
{
  while (bits--) {
    uint8_t mask = 0x80 >> (*pos & 7);
    if ((v >> bits) & 1) buf[*pos >> 3] |= mask;
    else buf[*pos >> 3] &= ~mask;
    (*pos)++;
  }
}

static uint32_t _get_bits(const uint8_t* buf, uint16_t pos, uint8_t bits)
{
  uint32_t v = 0;
  for (; bits; bits--, pos++) v = (v << 1) | ((buf[pos >> 3] >> (7 - (pos & 7))) & 1);
  return v;
}

// NRF CRC on the first 'bits' bits of buf
static uint16_t _nrf_crc_bits(const frame_t* f, const uint8_t* buf, uint16_t bits)
{
  uint16_t crc = f->crc_len == 2 ? NRF_CRC16_INIT : NRF_CRC8_INIT;
  uint16_t top = f->crc_len == 2 ? 0x8000 : 0x80;
  uint16_t poly = f->crc_len == 2 ? 0x1021 : 0x07;
  for (uint16_t i = 0; i < bits; i++) {
    bool msb = (crc & top) != 0;
    crc = (crc << 1) & (top | (top - 1));
    if (msb != ((buf[i >> 3] >> (7 - (i & 7))) & 1)) crc ^= poly;
  }
  return crc;
}

static uint8_t _scramble(const frame_t* f, uint8_t idx)
{
  return f->scramble ? _xn297_scramble[idx] : 0;
}

static uint16_t _xn297_xorout_for(const frame_t* f, uint8_t len)
{
  uint8_t idx = f->addr_len - 3 + len;
  if (f->enhanced)
    return f->scramble ? _xn297_xorout_scrambled_enhanced[idx] : _xn297_xorout_enhanced[idx];
  return f->scramble ? _xn297_xorout_scrambled[idx] : _xn297_xorout[idx];
}

// HS6200 packet control field and payload bits: the address before them
static uint16_t _hs6200_crc_bits(const frame_t* f, const uint8_t* buf, uint16_t end)
{
  uint16_t crc = _crc16_buf(NRF_CRC16_INIT, f->known + 1, f->addr_len);
  for (uint16_t i = 8 * f->hdr_len; i < end; i++) crc = _crc16(crc, _get_bits(buf, i, 1) << 7, 1);
  return crc;
}

static uint8_t _hs6200_scr(const frame_t* f, uint8_t idx)
{
  return f->type == FRAME_HS6200 && f->scramble ? _hs6200_scramble[idx] : 0;
}

// CYRF6936 data symbol length: TX_CFG.DATA_CODE_LENGTH
static uint8_t _cyrf_n(const frame_t* f) { return f->data32 ? 32 : 64; }

static uint64_t _chip_mask(uint8_t n) { return n == 64 ? ~0ULL : (1ULL << n) - 1; }

// CYRF6936 chips: bit k = chip k (code bytes sent LSB first), n chips
static uint64_t _cyrf_code(const uint8_t* code, uint8_t n)
{
  uint64_t c = 0;
  for (int8_t i = n / 8 - 1; i >= 0; i--) c = (c << 8) | code[i];
  return c;
}

static uint64_t _rotate(uint64_t c, uint8_t r, uint8_t n)
{
  return r ? ((c >> r) | (c << (n - r))) & _chip_mask(n) : c;
}

// 8DR symbol: data code (64 chips: 2 codes, bit 6; 32 chips: 4 codes, bits 5-6)
// rotated by the low bits, inverted if bit 7
static uint64_t _cyrf_symbol(const frame_t* f, uint8_t v)
{
  uint8_t n = _cyrf_n(f);
  uint8_t code = (v & 0x7F) / n;
  uint64_t c = _rotate(_cyrf_code(f->data_code + code * n / 8, n), v & (n - 1), n);
  return v & 0x80 ? ~c & _chip_mask(n) : c;
}

// TX_OVERRIDE.TX_INV: all chips inverted, preamble included (measured)
static void _put_chips(const frame_t* f, uint8_t* buf, uint16_t* pos, uint64_t chips, uint8_t n)
{
  for (uint8_t k = 0; k < n; k++) _put_bits(buf, pos, !((chips >> k) & 1) ^ f->invert, 1);
}

static uint64_t _get_chips(const frame_t* f, const uint8_t* buf, uint16_t pos, uint8_t n)
{
  uint64_t chips = 0;
  for (uint8_t k = 0; k < n; k++)
    if (!_get_bits(buf, pos + k, 1) ^ f->invert) chips |= 1ULL << k;
  return chips;
}

// Nearest symbol value
static uint8_t _cyrf_decode_symbol(const frame_t* f, uint64_t chips)
{
  uint8_t n = _cyrf_n(f);
  uint8_t best = 0, best_d = 0xFF;
  for (uint8_t code = 0; code < 128 / n; code++) {
    uint64_t c = _cyrf_code(f->data_code + code * n / 8, n);
    for (uint8_t r = 0; r < n; r++) {
      uint8_t d = __builtin_popcountll(chips ^ _rotate(c, r, n));
      uint8_t v = code * n + r;
      if (d > n / 2) {
        d = n - d;
        v |= 0x80;
      }
      if (d < best_d) {
        best_d = d;
        best = v;
      }
    }
  }
  return best;
}

static uint16_t _cyrf_crc(const frame_t* f, const uint8_t* data, uint8_t len)
{
  uint16_t crc = 0;
  for (uint8_t i = 0; i < 16; i++)
    if (f->crc_seed & (1 << i)) crc |= 0x8000 >> i;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++) crc = crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc ^ 0xFFFF;
}

// CYRF6936 gap chip after the header: the last header chip inverted (measured)
static uint8_t _cyrf_gap(const frame_t* f) { return !_get_bits(f->known, 8 * f->hdr_len - 1, 1); }

// CYRF6936: chips for n bytes (length, payload, CRC)
static uint16_t _cyrf_chips(const frame_t* f, uint8_t n)
{
  return (f->sdr ? 8 * n : n) * _cyrf_n(f);
}

// CYRF6936: bytes after the header for n bytes
static uint16_t _cyrf_air_len(const frame_t* f, uint8_t n)
{
  return (CYRF_GAP_CHIPS + _cyrf_chips(f, n) + 7) / 8;
}

// CC2500 FEC: data bytes encoded for n bytes of payload and CRC
static uint8_t _fec_data_len(uint8_t n) { return n + (n & 1 ? 1 : 2); }

// A7105: bytes after the ID for n bytes of payload and CRC
static uint8_t _a7105_air_len(const frame_t* f, uint8_t n)
{
  return f->fec ? (n * 14 + 7) / 8 : n;
}

// Total frame length for a payload of len bytes
static uint16_t _frame_len(const frame_t* f, uint8_t len)
{
  if (f->type == FRAME_A7105) return f->hdr_len + _a7105_air_len(f, len + f->crc_len);
  if (f->type == FRAME_CYRF) return f->hdr_len + _cyrf_air_len(f, f->var_len + len + f->crc_len);
  if (f->type == FRAME_CC2500 && f->fec) return f->hdr_len + 2 * _fec_data_len(len + f->crc_len);
  if (f->type == FRAME_XN297 && f->enhanced)
    return f->hdr_len + len + (f->crc_len ? 4 : 2);  // 10 bit pcf, CRC shifted by 2 bits
  if ((f->type == FRAME_NRF && f->esb) || f->type == FRAME_HS6200)
    return f->hdr_len + len + f->crc_len + 2;  // 9 bit pcf, CRC shifted by 1 bit
  if (f->type == FRAME_RF2500) return f->hdr_len + 4 * (len + f->crc_len);
  return f->hdr_len + len + f->crc_len;
}

bool frame_len_ok(const frame_t* f, uint8_t len)
{
  if (len > FRAME_PAYLOAD_MAX) return false;
  if (f->type == FRAME_NRF) return len <= FRAME_NRF_PAYLOAD_MAX;
  if (f->type == FRAME_CC2500 && f->fec)
    return _fec_data_len(len + f->crc_len) <= FRAME_FEC_DATA_MAX;
  if (f->type == FRAME_A7105) return _frame_len(f, len) <= FRAME_BUF_LEN - FRAME_PREFIX_MAX;
  if (f->type == FRAME_CYRF) return _frame_len(f, len) <= FRAME_AIR_MAX;
  if (f->type == FRAME_HS6200)
    return len <= (f->scramble ? sizeof(_hs6200_scramble) : FRAME_NRF_PAYLOAD_MAX);
  if (f->type == FRAME_RF2500) return !f->scramble || len == 16 || len == 19;
  if (f->type != FRAME_XN297) return true;
  // Scramble and xorout tables
  uint8_t extra = f->enhanced ? 2 : 0;
  return f->addr_len + len + extra <= sizeof(_xn297_scramble) &&
         f->addr_len - 3 + len < XN297_XOROUT_LEN;
}

//
// Encoders: full frame, returns its length
//

static uint8_t _encode_nrf(const frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* buf)
{
  uint8_t n = f->hdr_len;
  memcpy(buf + n, payload, len);
  n += len;

  if (f->crc_len == 2) {
    uint16_t crc = _crc16_buf(NRF_CRC16_INIT, buf, n);
    buf[n++] = crc >> 8;
    buf[n++] = crc & 0xFF;
  } else if (f->crc_len == 1) {
    buf[n] = _crc8_buf(NRF_CRC8_INIT, buf, n);
    n++;
  }
  return n;
}

// CRC of the ESB / HS6200 frame up to bit 'end'
static uint16_t _esb_crc(const frame_t* f, const uint8_t* buf, uint16_t end)
{
  return f->type == FRAME_HS6200 ? _hs6200_crc_bits(f, buf, end) : _nrf_crc_bits(f, buf, end);
}

// Enhanced ShockBurst and HS6200: packet control field, then the payload and
// CRC shifted by 1 bit
static uint8_t _encode_esb(const frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* buf)
{
  bool hs6200 = f->type == FRAME_HS6200;
  uint16_t pos = f->hdr_len * 8;
  _put_bits(buf, &pos, f->dpl || hs6200 ? len : ESB_STATIC_LEN, 6);
  _put_bits(buf, &pos, f->pid, 2);
  _put_bits(buf, &pos, hs6200 ? f->noack : !f->noack, 1);
  for (uint8_t i = 0; i < len; i++) _put_bits(buf, &pos, payload[i] ^ _hs6200_scr(f, i), 8);
  if (f->crc_len) _put_bits(buf, &pos, _esb_crc(f, buf, pos), f->crc_len * 8);
  while (pos & 7) _put_bits(buf, &pos, 0, 1);
  return pos / 8;
}

static uint8_t _encode_xn297(const frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* buf)
{
  uint8_t n = f->hdr_len;
  for (uint8_t i = 0; i < len; i++)
    buf[n++] = _bit_reverse(payload[i]) ^ _scramble(f, f->addr_len + i);

  if (f->crc_len) {
    uint16_t crc = _crc16_buf(XN297_CRC_INIT, buf + sizeof(_xn297_preamble),
                              n - sizeof(_xn297_preamble));
    crc ^= _xn297_xorout_for(f, len);
    buf[n++] = crc >> 8;
    buf[n++] = crc & 0xFF;
  }
  return n;
}

// Enhanced mode: 10 bit packet control field (6 bit length, 2 bit pid, no ack
// bit, 1 bit unused), then the payload and CRC shifted by 2 bits
static uint8_t _encode_xn297_enhanced(const frame_t* f, const uint8_t* payload, uint8_t len,
                                      uint8_t* buf)
{
  uint8_t s = f->addr_len;  // scramble index
  uint8_t n = f->hdr_len;

  buf[n++] = ((len << 1) | (f->pid >> 1)) ^ _scramble(f, s++);
  buf[n] = (f->pid << 7) | (f->noack << 6);
  if (len) {
    buf[n] |= _bit_reverse(payload[0]) >> 2;
    buf[n] ^= _scramble(f, s++);
    for (uint8_t i = 0; i < len - 1; i++) {
      n++;
      buf[n] = (_bit_reverse(payload[i]) << 6) | (_bit_reverse(payload[i + 1]) >> 2);
      buf[n] ^= _scramble(f, s++);
    }
    n++;
    buf[n] = _bit_reverse(payload[len - 1]) << 6;
  }
  buf[n] ^= _scramble(f, s) & 0xC0;

  if (!f->crc_len) return n + 1;

  uint16_t crc = _crc16_buf(XN297_CRC_INIT, buf + sizeof(_xn297_preamble),
                            n - sizeof(_xn297_preamble));
  crc = _crc16(crc, buf[n] & 0xC0, 2);
  crc ^= _xn297_xorout_for(f, len);
  buf[n++] |= (crc >> 8) >> 2;
  buf[n++] = ((crc >> 8) << 6) | ((crc & 0xFF) >> 2);
  buf[n++] = (crc & 0xFF) << 6;
  return n;
}

// CC2500 data whitening: PN9 (x^9 + x^5 + 1, all ones at start)
static void _pn9_whiten(uint8_t* data, uint8_t len)
{
  uint16_t reg = 0x1FF;
  for (uint8_t i = 0; i < len; i++) {
    data[i] ^= reg & 0xFF;
    for (int j = 0; j < 8; j++) reg = (reg >> 1) | (((reg ^ (reg >> 5)) & 1) << 8);
  }
}

// n data bytes (even) to 2n interleaved bytes
static void _fec_encode(const uint8_t* data, uint8_t n, uint8_t* out)
{
  uint16_t reg = 0;
  for (uint8_t i = 0; i < n; i += 2) {
    uint8_t fec[4];
    for (uint8_t k = 0; k < 2; k++) {
      reg = (reg & 0x700) | data[i + k];
      uint16_t sym = 0;
      for (int j = 0; j < 8; j++) {
        sym = (sym << 2) | _fec_table[reg >> 7];
        reg = (reg << 1) & 0x7FF;
      }
      fec[2 * k] = sym >> 8;
      fec[2 * k + 1] = sym & 0xFF;
    }
    uint32_t v = 0;
    for (int j = 0; j < 16; j++) v = (v << 2) | ((fec[~j & 3] >> (2 * (j >> 2))) & 3);
    for (int j = 0; j < 4; j++) out[2 * i + j] = v >> (24 - 8 * j);
  }
}

// 2n interleaved bytes to n data bytes: hard decision Viterbi decoder. The
// state is the last 3 data bits, newest in bit 0.
static void _fec_decode(const uint8_t* in, uint8_t n, uint8_t* data)
{
  uint8_t survivors[8 * FRAME_FEC_DATA_MAX];  // per bit: bit s = state s came from s>>1 | 4
  uint16_t metric[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint16_t bits = 0;

  for (uint16_t i = 0; i < 2 * n; i += 4) {
    uint32_t v = (uint32_t)in[i] << 24 | (uint32_t)in[i + 1] << 16 | in[i + 2] << 8 | in[i + 3];
    uint8_t fec[4] = {0};
    for (int j = 0; j < 16; j++) fec[~j & 3] |= ((v >> (30 - 2 * j)) & 3) << (2 * (j >> 2));

    for (int k = 0; k < 16; k++, bits++) {
      uint8_t sym = (fec[k >> 2] >> (6 - 2 * (k & 3))) & 3;
      uint16_t next[8];
      uint8_t surv = 0;
      for (uint8_t s = 0; s < 8; s++) {
        uint8_t p0 = s >> 1, p1 = p0 | 4, b = s & 1;
        uint8_t d0 = _fec_table[p0 << 1 | b] ^ sym, d1 = _fec_table[p1 << 1 | b] ^ sym;
        uint16_t m0 = metric[p0] + (d0 & 1) + (d0 >> 1);
        uint16_t m1 = metric[p1] + (d1 & 1) + (d1 >> 1);
        next[s] = m1 < m0 ? m1 : m0;
        if (m1 < m0) surv |= 1 << s;
      }
      survivors[bits] = surv;
      memcpy(metric, next, sizeof(metric));
    }
  }

  uint8_t s = 0;
  for (uint8_t i = 1; i < 8; i++)
    if (metric[i] < metric[s]) s = i;
  memset(data, 0, n);
  while (bits--) {
    data[bits >> 3] |= (s & 1) << (7 - (bits & 7));
    s = (survivors[bits] >> s) & 1 ? (s >> 1) | 4 : s >> 1;
  }
}

// CRC_EN with CC2400_EN (measured): CRC-16 0x8005 from 0 over all bytes but
// the last 2, which are added to it unprocessed
static uint16_t _cc2500_crc(const frame_t* f, const uint8_t* data, uint8_t len)
{
  if (!f->cc2400_crc) return _cc2500_crc_buf(CC2500_CRC_INIT, data, len);
  if (len < 2) return 0;
  return _cc2500_crc_buf(0, data, len - 2) ^ (data[len - 2] << 8 | data[len - 1]);
}

// The payload includes the length byte with a variable length
static uint8_t _encode_cc2500(const frame_t* f, const uint8_t* payload, uint8_t len,
                              uint8_t* buf)
{
  uint8_t body[FRAME_BUF_LEN];
  uint8_t n = len;
  memcpy(body, payload, len);

  if (f->crc_len) {
    uint16_t crc = _cc2500_crc(f, payload, len);
    body[n++] = crc >> 8;
    body[n++] = crc & 0xFF;
  }
  if (f->whitening) _pn9_whiten(body, n);

  uint8_t* air = buf + f->hdr_len;
  if (f->fec) {
    uint8_t m = _fec_data_len(n);
    memset(body + n, CC2500_FEC_TERM, m - n);
    _fec_encode(body, m, air);
    n = 2 * m;
  } else {
    memcpy(air, body, n);
  }
  if (f->invert)
    for (uint8_t i = 0; i < n; i++) air[i] ^= 0xFF;
  return f->hdr_len + n;
}

static uint8_t _encode_a7105(const frame_t* f, const uint8_t* payload, uint8_t len,
                             uint8_t* buf)
{
  uint8_t body[FRAME_BUF_LEN];
  uint8_t n = len;
  memcpy(body, payload, len);
  if (f->crc_len) {
    uint16_t crc = _crc16_buf(A7105_CRC_INIT, payload, len);
    body[n++] = crc >> 8;
    body[n++] = crc & 0xFF;
  }

  uint8_t* air = buf + f->hdr_len;
  if (!f->fec) {
    memcpy(air, body, n);
    return f->hdr_len + n;
  }
  uint16_t pos = 0;
  for (uint8_t i = 0; i < n; i++) {
    _put_bits(air, &pos, _a7105_fec[body[i] >> 4], 7);
    _put_bits(air, &pos, _a7105_fec[body[i] & 0x0F], 7);
  }
  while (pos & 7) _put_bits(air, &pos, 0, 1);
  return f->hdr_len + pos / 8;
}

// Payload (scrambled) and CRC bytes, as the bits to send, LSB first
static uint8_t _rf2500_body(const frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* body)
{
  uint8_t n = len;
  for (uint8_t i = 0; i < len; i++) body[i] = payload[i] ^ (f->scramble ? _rf2500_scramble[i] : 0);
  if (f->crc_len) {
    uint16_t crc = 0;
    for (uint8_t i = 0; i < len; i++) crc = _crc16(crc, _bit_reverse(body[i]), 8);
    uint16_t xorout = !f->scramble ? 0 : len == 16 ? RF2500_XOROUT_16 : RF2500_XOROUT_19;
    body[n++] = _bit_reverse(crc >> 8) ^ (xorout >> 8);
    body[n++] = _bit_reverse(crc & 0xFF) ^ (xorout & 0xFF);
  }
  return n;
}

static uint16_t _encode_rf2500(const frame_t* f, const uint8_t* payload, uint8_t len,
                               uint8_t* buf)
{
  uint8_t body[FRAME_BUF_LEN];
  uint8_t n = _rf2500_body(f, payload, len, body);
  uint16_t pos = 8 * f->hdr_len;
  for (uint8_t i = 0; i < n; i++)
    for (uint8_t b = 0; b < 8; b++)
      _put_bits(buf, &pos, (body[i] >> b) & 1 ? RF2500_ONE : RF2500_ZERO, 4);
  return pos / 8;
}

static uint16_t _encode_cyrf(const frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* buf)
{
  uint8_t body[FRAME_BUF_LEN];
  uint8_t n = 0;
  if (f->var_len) body[n++] = len;
  memcpy(body + n, payload, len);
  n += len;
  if (f->crc_len) {
    uint16_t crc = _cyrf_crc(f, body, n);
    body[n++] = crc & 0xFF;
    body[n++] = crc >> 8;
  }

  uint16_t pos = 8 * f->hdr_len;
  uint64_t code = _cyrf_code(f->data_code, CYRF_SDR_CHIPS);
  _put_bits(buf, &pos, _cyrf_gap(f), CYRF_GAP_CHIPS);
  for (uint8_t i = 0; i < n; i++) {
    if (!f->sdr) {
      _put_chips(f, buf, &pos, _cyrf_symbol(f, body[i]), _cyrf_n(f));
      continue;
    }
    for (uint8_t b = 0; b < 8; b++)
      _put_chips(f, buf, &pos, (body[i] >> b) & 1 ? ~code : code, CYRF_SDR_CHIPS);
  }
  while (pos & 7) _put_bits(buf, &pos, 1, 1);  // idle chips (0)
  return pos / 8;
}

static uint16_t _encode(const frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* buf)
{
  memcpy(buf, f->known, f->hdr_len);
  switch (f->type) {
    case FRAME_NRF:
      if (f->esb) return _encode_esb(f, payload, len, buf);
      return _encode_nrf(f, payload, len, buf);
    case FRAME_XN297:
      if (f->enhanced) return _encode_xn297_enhanced(f, payload, len, buf);
      return _encode_xn297(f, payload, len, buf);
    case FRAME_CC2500:
      return _encode_cc2500(f, payload, len, buf);
    case FRAME_A7105:
      return _encode_a7105(f, payload, len, buf);
    case FRAME_CYRF:
      return _encode_cyrf(f, payload, len, buf);
    case FRAME_HS6200:
      return _encode_esb(f, payload, len, buf);
    case FRAME_RF2500:
      return _encode_rf2500(f, payload, len, buf);
    default:
      memcpy(buf + f->hdr_len, payload, len);
      return f->hdr_len + len;
  }
}

//
// Decoders: full frame (known bytes + received bytes)
//

static frame_result_t _decode_nrf(const frame_t* f, const uint8_t* buf, uint8_t len,
                                  uint8_t* payload)
{
  memcpy(payload, buf + f->hdr_len, len);

  // Encode again and compare the CRC
  uint8_t check[FRAME_BUF_LEN];
  memcpy(check, buf, f->hdr_len);
  uint8_t n = _encode_nrf(f, payload, len, check);
  return memcmp(check + n - f->crc_len, buf + n - f->crc_len, f->crc_len) ? FRAME_BAD
                                                                           : FRAME_OK;
}

// 'len' is the maximum length with dynamic payload length
static frame_result_t _decode_esb(const frame_t* f, const uint8_t* buf, uint16_t avail,
                                  uint8_t len, uint8_t* payload, uint8_t* payload_len,
                                  uint16_t* pcf)
{
  uint16_t pos = f->hdr_len * 8;
  *pcf = _get_bits(buf, pos, ESB_PCF_BITS);
  pos += ESB_PCF_BITS;
  uint8_t size = len;
  if (f->dpl || f->type == FRAME_HS6200) {
    size = *pcf >> 3;
    if (size > len) {
      *payload_len = 0;
      return FRAME_BAD;
    }
  }
  if (avail < _frame_len(f, size)) return FRAME_SHORT;

  for (uint8_t i = 0; i < size; i++, pos += 8)
    payload[i] = _get_bits(buf, pos, 8) ^ _hs6200_scr(f, i);
  *payload_len = size;

  if (!f->crc_len) return FRAME_OK;
  return _get_bits(buf, pos, f->crc_len * 8) == _esb_crc(f, buf, pos) ? FRAME_OK : FRAME_BAD;
}

static frame_result_t _decode_xn297(const frame_t* f, const uint8_t* buf, uint8_t len,
                                    uint8_t* payload)
{
  for (uint8_t i = 0; i < len; i++)
    payload[i] = _bit_reverse(buf[f->hdr_len + i] ^ _scramble(f, f->addr_len + i));

  if (!f->crc_len) return FRAME_OK;

  uint16_t crc = _crc16_buf(XN297_CRC_INIT, buf + sizeof(_xn297_preamble),
                            f->hdr_len - sizeof(_xn297_preamble) + len);
  crc ^= _xn297_xorout_for(f, len);
  const uint8_t* rx_crc = buf + f->hdr_len + len;
  return rx_crc[0] == crc >> 8 && rx_crc[1] == (crc & 0xFF) ? FRAME_OK : FRAME_BAD;
}

// 'len' is the maximum length, the actual one comes from the packet control field
static frame_result_t _decode_xn297_enhanced(const frame_t* f, const uint8_t* buf,
                                             uint16_t avail, uint8_t len, uint8_t* payload,
                                             uint8_t* payload_len)
{
  const uint8_t* b = buf + f->hdr_len;
  uint8_t s = f->addr_len;
  uint8_t size = (b[0] ^ _scramble(f, s)) >> 1;
  if (size > len) {
    *payload_len = 0;
    return FRAME_BAD;
  }
  if (avail < _frame_len(f, size)) return FRAME_SHORT;

  for (uint8_t i = 0; i < size; i++) {
    uint8_t v = (b[i + 1] << 2) | (b[i + 2] >> 6);
    uint8_t scr = (_scramble(f, s + i + 1) << 2) | (_scramble(f, s + i + 2) >> 6);
    payload[i] = _bit_reverse(v) ^ _bit_reverse(scr);
  }
  *payload_len = size;

  if (!f->crc_len) return FRAME_OK;

  uint16_t crc = _crc16_buf(XN297_CRC_INIT, buf + sizeof(_xn297_preamble),
                            f->hdr_len - sizeof(_xn297_preamble) + size + 1);
  crc = _crc16(crc, b[size + 1] & 0xC0, 2);
  crc ^= _xn297_xorout_for(f, size);
  uint16_t rx_crc = (b[size + 1] << 10) | (b[size + 2] << 2) | (b[size + 3] >> 6);
  return crc == rx_crc ? FRAME_OK : FRAME_BAD;
}

// 'len' is the maximum length with a variable length
static frame_result_t _decode_cc2500(const frame_t* f, const uint8_t* buf, uint16_t avail,
                                     uint8_t len, uint8_t* payload, uint8_t* payload_len)
{
  uint8_t b[FRAME_BUF_LEN];
  uint8_t n = avail - f->hdr_len;
  memcpy(b, buf + f->hdr_len, n);
  if (f->invert)
    for (uint8_t i = 0; i < n; i++) b[i] ^= 0xFF;
  if (f->fec) {
    if (avail < _frame_len(f, len)) return FRAME_SHORT;
    uint8_t m = _fec_data_len(len + f->crc_len);
    if (f->var_len) {
      // Length byte first (first PN9 byte: 0xFF), then the packet only: noise
      // follows it
      uint8_t first[2];
      _fec_decode(b, 2, first);
      uint8_t size = (f->whitening ? first[0] ^ 0xFF : first[0]) + 1;
      if (size <= len) m = _fec_data_len(size + f->crc_len);
    }
    _fec_decode(b, m, b);
    n = m;
  }
  if (f->whitening) _pn9_whiten(b, n);

  uint8_t size = len;
  if (f->var_len) {
    if (n == 0) return FRAME_SHORT;
    size = b[0] + 1;
    if (size > len) {
      *payload_len = 0;
      return FRAME_BAD;
    }
  }
  if (avail < _frame_len(f, size)) return FRAME_SHORT;

  memcpy(payload, b, size);
  *payload_len = size;

  if (!f->crc_len) return FRAME_OK;
  uint16_t crc = _cc2500_crc(f, b, size);
  return b[size] == crc >> 8 && b[size + 1] == (crc & 0xFF) ? FRAME_OK : FRAME_BAD;
}

static frame_result_t _decode_a7105(const frame_t* f, const uint8_t* buf, uint8_t len,
                                    uint8_t* payload)
{
  const uint8_t* air = buf + f->hdr_len;
  uint8_t n = len + f->crc_len;
  uint8_t body[FRAME_BUF_LEN];
  if (f->fec) {
    // Nearest code word: corrects 1 bit error per nibble
    for (uint16_t i = 0; i < 2 * n; i++) {
      uint8_t w = _get_bits(air, i * 7, 7), best = 0, best_d = 8;
      for (uint8_t v = 0; v < 16; v++) {
        uint8_t d = __builtin_popcount(w ^ _a7105_fec[v]);
        if (d < best_d) {
          best_d = d;
          best = v;
        }
      }
      body[i / 2] = i & 1 ? body[i / 2] | best : best << 4;
    }
  } else {
    memcpy(body, air, n);
  }

  memcpy(payload, body, len);
  if (!f->crc_len) return FRAME_OK;
  uint16_t crc = _crc16_buf(A7105_CRC_INIT, body, len);
  return body[len] == crc >> 8 && body[len + 1] == (crc & 0xFF) ? FRAME_OK : FRAME_BAD;
}

// Nearest 4 bit code per bit (1 error corrected, 2: read as 0)
static frame_result_t _decode_rf2500(const frame_t* f, const uint8_t* buf, uint8_t len,
                                     uint8_t* payload)
{
  uint8_t body[FRAME_BUF_LEN], check[FRAME_BUF_LEN];
  uint8_t n = len + f->crc_len;
  uint16_t pos = 8 * f->hdr_len;
  for (uint8_t i = 0; i < n; i++) {
    body[i] = 0;
    for (uint8_t b = 0; b < 8; b++, pos += 4)
      if (__builtin_popcount(_get_bits(buf, pos, 4) ^ RF2500_ONE) < 2) body[i] |= 1 << b;
  }
  for (uint8_t i = 0; i < len; i++)
    payload[i] = body[i] ^ (f->scramble ? _rf2500_scramble[i] : 0);
  if (!f->crc_len) return FRAME_OK;
  _rf2500_body(f, payload, len, check);
  return memcmp(check + len, body + len, 2) ? FRAME_BAD : FRAME_OK;
}

// len: maximum payload length with a length field
static uint8_t _cyrf_decode_byte(const frame_t* f, const uint8_t* buf, uint16_t* pos)
{
  if (!f->sdr) {
    uint8_t v = _cyrf_decode_symbol(f, _get_chips(f, buf, *pos, _cyrf_n(f)));
    *pos += _cyrf_n(f);
    return v;
  }
  uint64_t code = _cyrf_code(f->data_code, CYRF_SDR_CHIPS);
  uint8_t v = 0;
  for (uint8_t b = 0; b < 8; b++, *pos += CYRF_SDR_CHIPS)
    if (__builtin_popcountll(_get_chips(f, buf, *pos, CYRF_SDR_CHIPS) ^ code) > 32) v |= 1 << b;
  return v;
}

static frame_result_t _decode_cyrf(const frame_t* f, const uint8_t* buf, uint16_t avail,
                                   uint8_t len, uint8_t* payload, uint8_t* payload_len)
{
  uint16_t pos = 8 * f->hdr_len + CYRF_GAP_CHIPS;
  uint8_t body[FRAME_BUF_LEN];
  uint8_t n = 0;
  if (f->var_len) {
    if (avail < f->hdr_len + _cyrf_air_len(f, 1)) return FRAME_SHORT;
    body[n++] = _cyrf_decode_byte(f, buf, &pos);
    if (body[0] > len) return FRAME_BAD;
    len = body[0];
  }
  uint8_t total = n + len + f->crc_len;
  if (avail < f->hdr_len + _cyrf_air_len(f, total)) return FRAME_SHORT;
  for (; n < total; n++) body[n] = _cyrf_decode_byte(f, buf, &pos);

  memcpy(payload, body + f->var_len, len);
  *payload_len = len;
  // Known first payload bytes (chip errors are allowed: not compared on air)
  if (f->prefix_len && (len < f->prefix_len || memcmp(payload, f->prefix, f->prefix_len)))
    return FRAME_BAD;
  if (!f->crc_len) return FRAME_OK;
  uint16_t crc = _cyrf_crc(f, body, f->var_len + len);
  return body[total - 2] == (crc & 0xFF) && body[total - 1] == crc >> 8 ? FRAME_OK : FRAME_BAD;
}

//
// API
//

static uint32_t _sync_word(const uint8_t* b, uint8_t len)
{
  uint32_t sw = 0;
  while (len--) sw = (sw << 8) | *b++;
  return sw;
}

// RX sync word choice. The RF core sync detection tolerates bit errors: a sync
// word that nearly matches the bits sent before it can trigger early (packet
// lost) or not at all. Measured with an nRF24L01 at 1 Mbps, address e7e7e7e7e7:
// 33% received with the sync on its first 4 bytes, 100% with the preamble and
// the first 3 bytes. So the sync word is the window of the known bytes (with
// the NRF preamble) that differs the most from the bits before it, the latest
// one when several are good enough.
#define SYNC_SCORE_GOOD 16  // in half bits: 8 bit errors, measured: 7 is enough

// Smallest distance between the window at byte 'pos' and the earlier bits, in
// half bits (bits before the stream are noise: 1/2 error). All the earlier
// bits: the CYRF6936 SOP code repeats 36 chips later
static uint8_t _sync_score(const uint8_t* stream, uint8_t pos, uint8_t len)
{
  uint8_t best = 0xFF;
  int16_t first = 8 * pos;
  int16_t from = first < 8 * len ? first - 8 * len + 1 : 0;
  for (int16_t at = from; at < first; at++) {
    uint8_t errors = 0;
    for (int16_t i = 0; i < 8 * len && errors < best; i++) {
      int16_t j = at + i;
      if (j < 0) errors += 1;
      else if (_get_bits(stream, first + i, 1) != _get_bits(stream, j, 1)) errors += 2;
    }
    if (errors < best) best = errors;
  }
  return best;
}

static void _choose_rx_sync(frame_t* f)
{
  // NRF: the 1 byte preamble before the address is known too
  uint8_t stream[1 + sizeof(f->known)];
  uint8_t lead = 0;
  if (f->type == FRAME_NRF) stream[lead++] = f->known[0] & 0x80 ? 0xAA : 0x55;
  memcpy(stream + lead, f->known, f->known_len);
  uint8_t stream_len = lead + f->known_len;

  uint8_t best_pos = stream_len - f->sync_len;
  uint8_t best_score = 0;
  for (uint8_t pos = 0; pos + f->sync_len <= stream_len; pos++) {
    uint8_t score = _sync_score(stream, pos, f->sync_len);
    if (score > SYNC_SCORE_GOOD) score = SYNC_SCORE_GOOD;
    if (score >= best_score) {
      best_score = score;
      best_pos = pos;
    }
  }
  f->rx_sync = _sync_word(stream + best_pos, f->sync_len);
  f->rx_sync_errors = best_score / 2;
  f->rx_start = best_pos + f->sync_len - lead;

  // Same sync word earlier in the frame (all windows repeat, e.g. address
  // 5555555555): receive enough for the earliest one
  f->rx_first_start = f->rx_start;
  for (uint8_t pos = lead; pos < best_pos; pos++) {
    if (_sync_word(stream + pos, f->sync_len) == f->rx_sync) {
      f->rx_first_start = pos + f->sync_len - lead;
      break;
    }
  }
}

bool frame_init(frame_t* f)
{
  uint8_t n = 0;

  switch (f->type) {
    case FRAME_RAW:
      if (f->addr_len < 1 || f->addr_len > 4) return false;
      memcpy(f->known, f->addr, f->addr_len);
      n = f->addr_len;
      f->sync_len = n;
      f->crc_len = 0;
      break;

    case FRAME_NRF:
      if (f->addr_len < 3 || f->addr_len > FRAME_ADDR_MAX || f->crc_len > 2 ||
          f->sync_bytes < 2 || f->sync_bytes > 4)
        return false;
      if (f->esb && f->prefix_len) return false;
      for (uint8_t i = 0; i < f->addr_len; i++) f->known[n++] = f->addr[f->addr_len - 1 - i];
      f->sync_len = f->addr_len < f->sync_bytes ? f->addr_len : f->sync_bytes;
      break;

    case FRAME_XN297:
      if (f->addr_len < 3 || f->addr_len > FRAME_ADDR_MAX || (f->crc_len && f->crc_len != 2) ||
          (f->enhanced && f->prefix_len))
        return false;
      memcpy(f->known, _xn297_preamble, sizeof(_xn297_preamble));
      n = sizeof(_xn297_preamble);
      for (uint8_t i = 0; i < f->addr_len; i++)
        f->known[n++] = f->addr[f->addr_len - 1 - i] ^ _scramble(f, i);
      f->sync_len = sizeof(_xn297_preamble);
      break;

    case FRAME_CC2500:
      if (f->addr_len < 1 || f->addr_len > 4 || (f->crc_len && f->crc_len != 2) ||
          (f->fec && f->prefix_len))
        return false;
      for (uint8_t i = 0; i < CC2500_PREAMBLE_LEN; i++) f->known[n++] = CC2500_PREAMBLE;
      memcpy(f->known + n, f->addr, f->addr_len);
      n += f->addr_len;
      if (f->invert)
        for (uint8_t i = 0; i < n; i++) f->known[i] ^= 0xFF;
      f->sync_len = 4;
      break;

    case FRAME_A7105:
      if ((f->addr_len != 2 && f->addr_len != 4) || (f->crc_len && f->crc_len != 2) ||
          (f->fec && f->prefix_len))
        return false;
      memcpy(f->known, f->addr, f->addr_len);
      n = f->addr_len;
      f->sync_len = n;
      break;

    case FRAME_CYRF: {
      // 8DR needs the SOP code, SDR 64 chip codes (as the MULTI-Module protocols)
      if ((f->crc_len && f->crc_len != 2) || (f->var_len && f->prefix_len) ||
          (!f->sop && !f->sdr) || (f->sdr && f->data32))
        return false;
      uint16_t pos = 0;
      uint8_t sop_n = f->sop32 ? 32 : 64;
      uint64_t sop = _cyrf_code(f->sop_code, sop_n);
      for (uint8_t i = 0; i < (f->sop ? CYRF_PREAMBLE_NIBBLES : 16); i++)
        _put_bits(f->known, &pos, f->invert ? 0x3 : 0xC, 4);
      if (f->sop) {
        // 4 alternating chips between the copies, from the last one inverted.
        // The second copy is inverted with 64 chip data codes only (measured)
        _put_chips(f, f->known, &pos, ~sop, sop_n);
        _put_bits(f->known, &pos, _get_bits(f->known, pos - 1, 1) ? 0x5 : 0xA, 4);
        _put_chips(f, f->known, &pos, f->data32 ? ~sop : sop, sop_n);
      }
      n = pos / 8;
      f->sync_len = 4;
      break;
    }

    case FRAME_HS6200:
      if (f->addr_len < 3 || f->addr_len > FRAME_ADDR_MAX || (f->crc_len && f->crc_len != 2) ||
          f->prefix_len)
        return false;
      f->known[n++] = GFSK1M_PREAMBLE;
      for (uint8_t i = 0; i < f->addr_len; i++) f->known[n++] = f->addr[f->addr_len - 1 - i];
      for (uint8_t i = 0; i < HS6200_GUARD_LEN; i++) f->known[n++] = f->addr[0];
      f->sync_len = 4;
      break;

    case FRAME_RF2500:
      if (f->addr_len != RF2500_ADDR_LEN || (f->crc_len && f->crc_len != 2) || f->prefix_len)
        return false;
      f->known[n++] = GFSK1M_PREAMBLE;
      memcpy(f->known + n, f->addr, RF2500_ADDR_LEN);
      n += RF2500_ADDR_LEN;
      f->known[n++] = RF2500_SYNC;
      f->known[n++] = RF2500_SYNC;
      f->sync_len = 4;
      break;

    default:
      return false;
  }
  f->hdr_len = n;

  // Known payload prefix, encoded as it will be sent. CYRF6936: the whole
  // bytes of its chips that fit
  if (f->prefix_len) {
    if (f->prefix_len > FRAME_PREFIX_MAX || !frame_len_ok(f, f->prefix_len)) return false;
    uint8_t buf[FRAME_AIR_MAX];
    _encode(f, f->prefix, f->prefix_len, buf);
    uint16_t known = f->prefix_len;
    if (f->type == FRAME_CYRF) known = (CYRF_GAP_CHIPS + _cyrf_chips(f, f->prefix_len)) / 8;
    if (n + known > sizeof(f->known)) known = sizeof(f->known) - n;
    memcpy(f->known + n, buf + n, known);
    n += known;
  }
  f->known_len = n;
  _choose_rx_sync(f);
  f->rx_sync_alt = 0;
  if (f->type == FRAME_CYRF && !f->sop && !f->prefix_len) {
    // Only the repeated preamble is known: the CC2652 misses most packets with
    // a sync word on it (measured). Sync on the preamble end, the gap and the
    // first chips of the first data bit instead: one sync word per bit value.
    // The first symbol is decoded with these known chips (23 of 64).
    uint16_t pos = 8 * n;
    uint64_t code = _cyrf_code(f->data_code, CYRF_SDR_CHIPS);  // bit 0
    _put_bits(f->known, &pos, _cyrf_gap(f), CYRF_GAP_CHIPS);
    _put_chips(f, f->known, &pos, code, CYRF_SYNC_CHIPS);
    f->known_len = pos / 8;
    f->rx_sync = _sync_word(f->known + f->known_len - f->sync_len, f->sync_len);
    f->rx_sync_alt = f->rx_sync ^ ((1u << CYRF_SYNC_CHIPS) - 1);
    // Weak: the default threshold loses 26 to 55% of the bind responses (data
    // code 98881be430790384), the strict one none (measured)
    f->rx_sync_errors = 0;
    f->rx_start = f->rx_first_start = f->known_len;
  }
  return true;
}

uint8_t frame_sync_bits(const frame_t* f) { return f->sync_len * 8; }

uint32_t frame_tx_sync(const frame_t* f) { return _sync_word(f->known, f->sync_len); }

uint32_t frame_rx_sync(const frame_t* f) { return f->rx_sync; }

uint32_t frame_rx_sync_alt(const frame_t* f) { return f->rx_sync_alt; }

bool frame_rx_sync_weak(const frame_t* f) { return f->rx_sync_errors * 2 < SYNC_SCORE_GOOD; }

static bool _has_pid(const frame_t* f)
{
  return (f->type == FRAME_XN297 && f->enhanced) || (f->type == FRAME_NRF && f->esb) ||
         f->type == FRAME_HS6200;
}

uint16_t frame_tx_data(frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* data)
{
  if (!frame_len_ok(f, len)) return 0;

  uint8_t buf[FRAME_AIR_MAX];
  uint16_t n = _encode(f, payload, len, buf) - f->sync_len;
  memcpy(data, buf + f->sync_len, n);
  f->tx_pid = f->pid;
  if (_has_pid(f)) f->pid = (f->pid + 1) & 0x03;
  return n;
}

uint16_t frame_rx_len(const frame_t* f, uint8_t len)
{
  if (!frame_len_ok(f, len) || len < f->prefix_len) return 0;
  return _frame_len(f, len) - f->rx_first_start;
}

static frame_result_t _decode_at(const frame_t* f, uint8_t start, const uint8_t* data,
                                 uint16_t rx_len, uint8_t len, uint8_t* payload,
                                 uint8_t* payload_len, uint16_t* pcf)
{
  uint8_t buf[FRAME_AIR_MAX];
  uint16_t avail = start + rx_len;
  if (avail > sizeof(buf)) avail = sizeof(buf);
  memcpy(buf, f->known, start);
  memcpy(buf + start, data, avail - start);

  frame_result_t res;
  *payload_len = len;
  *pcf = 0;
  if (_has_pid(f)) {
    if (avail < f->hdr_len + 2) return FRAME_SHORT;
    if (f->type == FRAME_XN297) res = _decode_xn297_enhanced(f, buf, avail, len, payload, payload_len);
    else res = _decode_esb(f, buf, avail, len, payload, payload_len, pcf);
  } else if (f->type == FRAME_CC2500) {
    res = _decode_cc2500(f, buf, avail, len, payload, payload_len);
  } else if (f->type == FRAME_CYRF) {
    // Synced with either polarity of the first data bit: the known chips must
    // match the received ones
    uint16_t sym = 8 * f->hdr_len + CYRF_GAP_CHIPS;
    if (f->rx_sync_alt && 8 * avail >= sym + CYRF_SDR_CHIPS) {
      uint64_t diff =
          _get_chips(f, buf, sym, CYRF_SDR_CHIPS) ^ _cyrf_code(f->data_code, CYRF_SDR_CHIPS);
      if (__builtin_popcountll(diff >> CYRF_SYNC_CHIPS) > (CYRF_SDR_CHIPS - CYRF_SYNC_CHIPS) / 2)
        for (uint16_t p = sym; p < sym + CYRF_SYNC_CHIPS; p++) buf[p >> 3] ^= 0x80 >> (p & 7);
    }
    res = _decode_cyrf(f, buf, avail, len, payload, payload_len);
  } else {
    if (avail < _frame_len(f, len)) return FRAME_SHORT;
    if (f->type == FRAME_NRF) res = _decode_nrf(f, buf, len, payload);
    else if (f->type == FRAME_XN297) res = _decode_xn297(f, buf, len, payload);
    else if (f->type == FRAME_A7105) res = _decode_a7105(f, buf, len, payload);
    else if (f->type == FRAME_RF2500) res = _decode_rf2500(f, buf, len, payload);
    else {
      memcpy(payload, buf + f->hdr_len, len);
      res = FRAME_OK;
    }
  }

  // The received known bytes must be right too (CYRF6936: decoded instead)
  if (res == FRAME_OK && f->type != FRAME_CYRF && start < f->known_len &&
      memcmp(buf + start, f->known + start, f->known_len - start))
    res = FRAME_BAD;
  return res;
}

frame_result_t frame_decode_pcf(const frame_t* f, const uint8_t* data, uint16_t rx_len,
                                uint8_t len, uint8_t* payload, uint8_t* payload_len,
                                uint16_t* pcf)
{
  // Normal sync position first, then earlier copies of the sync word
  frame_result_t res = _decode_at(f, f->rx_start, data, rx_len, len, payload, payload_len, pcf);
  for (uint8_t start = f->rx_start - 1; start >= f->rx_first_start && res != FRAME_OK; start--) {
    if (start < f->sync_len ||
        _sync_word(f->known + start - f->sync_len, f->sync_len) != f->rx_sync)
      continue;
    uint8_t other[FRAME_PAYLOAD_MAX];
    uint8_t other_len;
    uint16_t other_pcf;
    if (_decode_at(f, start, data, rx_len, len, other, &other_len, &other_pcf) == FRAME_OK) {
      memcpy(payload, other, other_len);
      *payload_len = other_len;
      *pcf = other_pcf;
      res = FRAME_OK;
    }
  }
  return res;
}

frame_result_t frame_decode(const frame_t* f, const uint8_t* data, uint16_t rx_len,
                            uint8_t len, uint8_t* payload, uint8_t* payload_len)
{
  uint16_t pcf;
  return frame_decode_pcf(f, data, rx_len, len, payload, payload_len, &pcf);
}
