#include <string.h>

#include "frame.h"

// Frame buffer: header + prefix + longest encoded payload + CRC
#define FRAME_BUF_LEN (FRAME_HDR_MAX + FRAME_PREFIX_MAX + FRAME_PAYLOAD_MAX + 8)

#define NRF_CRC16_INIT 0xFFFF
#define NRF_CRC8_INIT 0xFF
#define XN297_CRC_INIT 0xB5D2
#define CC2500_CRC_INIT 0xFFFF

// CC2500: last preamble bytes before the sync word (at least 2 are sent)
#define CC2500_PREAMBLE 0xAA
#define CC2500_PREAMBLE_LEN 2

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

// Total frame length for a payload of len bytes
static uint8_t _frame_len(const frame_t* f, uint8_t len)
{
  if (f->type == FRAME_XN297 && f->enhanced)
    return f->hdr_len + len + (f->crc_len ? 4 : 2);  // 10 bit pcf, CRC shifted by 2 bits
  if (f->type == FRAME_NRF && f->esb)
    return f->hdr_len + len + f->crc_len + 2;  // 9 bit pcf, CRC shifted by 1 bit
  return f->hdr_len + len + f->crc_len;
}

bool frame_len_ok(const frame_t* f, uint8_t len)
{
  if (len > FRAME_PAYLOAD_MAX) return false;
  if (f->type == FRAME_NRF) return len <= FRAME_NRF_PAYLOAD_MAX;
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

// Enhanced ShockBurst: packet control field, then the payload and CRC shifted
// by 1 bit
static uint8_t _encode_esb(const frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* buf)
{
  uint16_t pos = f->hdr_len * 8;
  _put_bits(buf, &pos, f->dpl ? len : ESB_STATIC_LEN, 6);
  _put_bits(buf, &pos, f->pid, 2);
  _put_bits(buf, &pos, !f->noack, 1);
  for (uint8_t i = 0; i < len; i++) _put_bits(buf, &pos, payload[i], 8);
  if (f->crc_len) _put_bits(buf, &pos, _nrf_crc_bits(f, buf, pos), f->crc_len * 8);
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

// The payload includes the length byte with a variable length
static uint8_t _encode_cc2500(const frame_t* f, const uint8_t* payload, uint8_t len,
                              uint8_t* buf)
{
  uint8_t n = f->hdr_len;
  memcpy(buf + n, payload, len);
  n += len;

  if (f->crc_len) {
    uint16_t crc = _cc2500_crc_buf(CC2500_CRC_INIT, payload, len);
    buf[n++] = crc >> 8;
    buf[n++] = crc & 0xFF;
  }
  return n;
}

static uint8_t _encode(const frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* buf)
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
static frame_result_t _decode_esb(const frame_t* f, const uint8_t* buf, uint8_t avail,
                                  uint8_t len, uint8_t* payload, uint8_t* payload_len,
                                  uint16_t* pcf)
{
  uint16_t pos = f->hdr_len * 8;
  *pcf = _get_bits(buf, pos, ESB_PCF_BITS);
  pos += ESB_PCF_BITS;
  uint8_t size = len;
  if (f->dpl) {
    size = *pcf >> 3;
    if (size > len) {
      *payload_len = 0;
      return FRAME_BAD;
    }
  }
  if (avail < _frame_len(f, size)) return FRAME_SHORT;

  for (uint8_t i = 0; i < size; i++, pos += 8) payload[i] = _get_bits(buf, pos, 8);
  *payload_len = size;

  if (!f->crc_len) return FRAME_OK;
  return _get_bits(buf, pos, f->crc_len * 8) == _nrf_crc_bits(f, buf, pos) ? FRAME_OK
                                                                           : FRAME_BAD;
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
                                             uint8_t avail, uint8_t len, uint8_t* payload,
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
static frame_result_t _decode_cc2500(const frame_t* f, const uint8_t* buf, uint8_t avail,
                                     uint8_t len, uint8_t* payload, uint8_t* payload_len)
{
  const uint8_t* b = buf + f->hdr_len;
  uint8_t size = len;
  if (f->var_len) {
    if (avail <= f->hdr_len) return FRAME_SHORT;
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
  uint16_t crc = _cc2500_crc_buf(CC2500_CRC_INIT, b, size);
  return b[size] == crc >> 8 && b[size + 1] == (crc & 0xFF) ? FRAME_OK : FRAME_BAD;
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
// half bits (bits before the stream are noise: 1/2 error)
static uint8_t _sync_score(const uint8_t* stream, uint8_t pos, uint8_t len)
{
  uint8_t best = 0xFF;
  int16_t first = 8 * pos;
  for (int16_t at = first - 8 * len + 1; at < first; at++) {
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
      if (f->addr_len < 1 || f->addr_len > 4 || (f->crc_len && f->crc_len != 2)) return false;
      for (uint8_t i = 0; i < CC2500_PREAMBLE_LEN; i++) f->known[n++] = CC2500_PREAMBLE;
      memcpy(f->known + n, f->addr, f->addr_len);
      n += f->addr_len;
      f->sync_len = 4;
      break;

    default:
      return false;
  }
  f->hdr_len = n;

  // Known payload prefix, encoded as it will be sent
  if (f->prefix_len) {
    if (f->prefix_len > FRAME_PREFIX_MAX || !frame_len_ok(f, f->prefix_len)) return false;
    uint8_t buf[FRAME_BUF_LEN];
    _encode(f, f->prefix, f->prefix_len, buf);
    memcpy(f->known + n, buf + n, f->prefix_len);
    n += f->prefix_len;
  }
  f->known_len = n;
  _choose_rx_sync(f);
  return true;
}

uint8_t frame_sync_bits(const frame_t* f) { return f->sync_len * 8; }

uint32_t frame_tx_sync(const frame_t* f) { return _sync_word(f->known, f->sync_len); }

uint32_t frame_rx_sync(const frame_t* f) { return f->rx_sync; }

bool frame_rx_sync_weak(const frame_t* f) { return f->rx_sync_errors * 2 < SYNC_SCORE_GOOD; }

static bool _has_pid(const frame_t* f)
{
  return (f->type == FRAME_XN297 && f->enhanced) || (f->type == FRAME_NRF && f->esb);
}

uint8_t frame_tx_data(frame_t* f, const uint8_t* payload, uint8_t len, uint8_t* data)
{
  if (!frame_len_ok(f, len)) return 0;

  uint8_t buf[FRAME_BUF_LEN];
  uint8_t n = _encode(f, payload, len, buf) - f->sync_len;
  memcpy(data, buf + f->sync_len, n);
  f->tx_pid = f->pid;
  if (_has_pid(f)) f->pid = (f->pid + 1) & 0x03;
  return n;
}

uint8_t frame_rx_len(const frame_t* f, uint8_t len)
{
  if (!frame_len_ok(f, len) || len < f->prefix_len) return 0;
  return _frame_len(f, len) - f->rx_first_start;
}

static frame_result_t _decode_at(const frame_t* f, uint8_t start, const uint8_t* data,
                                 uint8_t rx_len, uint8_t len, uint8_t* payload,
                                 uint8_t* payload_len, uint16_t* pcf)
{
  uint8_t buf[FRAME_BUF_LEN];
  uint8_t avail = start + rx_len;
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
  } else {
    if (avail < _frame_len(f, len)) return FRAME_SHORT;
    if (f->type == FRAME_NRF) res = _decode_nrf(f, buf, len, payload);
    else if (f->type == FRAME_XN297) res = _decode_xn297(f, buf, len, payload);
    else {
      memcpy(payload, buf + f->hdr_len, len);
      res = FRAME_OK;
    }
  }

  // The received known bytes must be right too
  if (res == FRAME_OK && start < f->known_len &&
      memcmp(buf + start, f->known + start, f->known_len - start))
    res = FRAME_BAD;
  return res;
}

frame_result_t frame_decode_pcf(const frame_t* f, const uint8_t* data, uint8_t rx_len,
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

frame_result_t frame_decode(const frame_t* f, const uint8_t* data, uint8_t rx_len,
                            uint8_t len, uint8_t* payload, uint8_t* payload_len)
{
  uint16_t pcf;
  return frame_decode_pcf(f, data, rx_len, len, payload, payload_len, &pcf);
}
