// HS6200 and RF2500 framings vs the MULTI-Module HS6200_EMU.ino /
// RF2500_EMU.ino code (before its bit_reverse() for the CYRF6936)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint16_t crc;
static void crc16_update(uint8_t a, uint8_t bits)
{
  crc ^= a << 8;
  while (bits--) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
}
static uint8_t bit_reverse(uint8_t b) { uint8_t r = 0; for (int i = 0; i < 8; i++, b >>= 1) r = (r << 1) | (b & 1); return r; }

// HS6200_EMU.ino, 'fixed': last CRC bit from msg[len+1] instead of msg[len+2]
static uint8_t hs6200(const uint8_t* addr, uint8_t alen, const uint8_t* msg, uint8_t len, uint8_t pid,
                      bool fixed, uint8_t garbage, uint8_t* payload)
{
  static const uint8_t S[] = {0x80,0xf5,0x3b,0x0d,0x6d,0x2a,0xf9,0xbc,0x51,0x8e,0x4c,0xfd,0xc1,0x65,0xd0};
  memset(payload, garbage, 32);
  crc = 0xffff;
  for (uint8_t i = 0; i < alen; i++) crc16_update(addr[alen - 1 - i], 8);
  uint16_t crc_init = crc;
  uint8_t pos = 0;
  for (int8_t i = alen - 1; i >= 0; i--) payload[pos++] = addr[i];
  payload[pos++] = addr[0];
  payload[pos++] = addr[0];
  payload[pos++] = ((len & 0x3f) << 2) | (pid & 0x03);
  payload[pos] = 1 << 7;
  if (len > 0) {
    payload[pos++] |= (msg[0] ^ S[0]) >> 1;
    for (uint8_t i = 1; i < len; i++) payload[pos++] = ((msg[i-1] ^ S[i-1]) << 7) | ((msg[i] ^ S[i]) >> 1);
    payload[pos] = (msg[len-1] ^ S[len-1]) << 7;
  }
  uint8_t* m = &payload[alen + 2];
  uint8_t l = len + 2, p;
  crc = crc_init;
  for (p = 0; p < l - 1; p++) crc16_update(m[p], 8);
  crc16_update(m[fixed ? p : p + 1], 1);
  payload[pos++] |= (crc >> 8) >> 1;
  payload[pos++] = ((crc >> 8) << 7) | ((crc & 0xff) >> 1);
  payload[pos++] = crc << 7;
  return pos;
}

static uint8_t rf2500(const uint8_t* addr, uint8_t* buffer, uint8_t len, bool scramble, uint8_t* out)
{
  const uint8_t S[] = {0xD0,0x9E,0x53,0x33,0xD8,0xBA,0x98,0x08,0x24,0xCB,0x3B,0xFC,0x71,0xA3,0xF4,0x55,0x68,0x4F,0xA9};
  uint16_t xo = len == 16 ? 0xAEE4 : 0xE7C5;
  if (scramble) for (uint8_t i = 0; i < len; i++) buffer[i] ^= S[i];
  crc = 0;
  for (uint8_t i = 0; i < len; i++) crc16_update(bit_reverse(buffer[i]), 8);
  buffer[len] = bit_reverse(crc >> 8);
  buffer[len + 1] = bit_reverse(crc);
  if (scramble) { buffer[len] ^= xo >> 8; buffer[len + 1] ^= xo; }
  memcpy(out, addr, 4);
  uint8_t pos = 4;
  out[pos++] = 0xC3; out[pos++] = 0xC3;
  memset(&out[pos], 0, (len + 2) * 4);
  for (uint8_t i = 0; i < len + 2; i++)
    for (uint8_t j = 0; j < 8; j++) {
      uint8_t o = pos + (i << 2) + (j >> 1);
      out[o] <<= 4;
      out[o] |= (buffer[i] >> j) & 1 ? 0x0C : 0x03;
    }
  return 4 + 2 + (len + 2) * 4;
}

// Full frame: TX sync word + data
static uint16_t full(frame_t* f, const uint8_t* p, uint8_t len, uint8_t* out)
{
  uint32_t sw = frame_tx_sync(f);
  uint8_t n = frame_sync_bits(f) / 8;
  for (uint8_t i = 0; i < n; i++) out[i] = sw >> (8 * (n - 1 - i));
  return n + frame_tx_data(f, p, len, out + n);
}

static bool roundtrip(frame_t* f, const uint8_t* frame, uint16_t flen, const uint8_t* p, uint8_t len)
{
  uint8_t out[64], out_len;
  uint16_t rx = frame_rx_len(f, len);
  uint8_t start = f->rx_start;
  if (rx == 0 || start + rx > flen + 1) return false;
  return frame_decode(f, frame + start, flen - start, len, out, &out_len) == FRAME_OK &&
         out_len == len && !memcmp(out, p, len);
}

int main()
{
  srand(1);
  uint8_t mpm[128], cc[1200];
  // HS6200: E012 / E015 bind addresses, random payloads up to 15 bytes
  const uint8_t* addrs[] = {(const uint8_t*)"\x55\x42\x9C\x8F\xC9", (const uint8_t*)"\x62\x54\x79\x38\x53"};
  int hs_ok = 0, hs_bug_same = 0, hs_n = 0;
  for (int a = 0; a < 2; a++) {
    frame_t f = {.type = FRAME_HS6200, .addr_len = 5, .crc_len = 2, .scramble = true, .noack = true};
    memcpy(f.addr, addrs[a], 5);
    CHECK(frame_init(&f));
    for (int t = 0; t < 200; t++) {
      uint8_t len = 1 + rand() % 15, p[16];
      for (int i = 0; i < len; i++) p[i] = rand();
      uint8_t pid = f.pid;
      uint16_t n = full(&f, p, len, cc);
      uint8_t m = hs6200(addrs[a], 5, p, len, pid, true, 0, mpm);
      bool same = cc[0] == 0xAA && n == m + 1 && !memcmp(cc + 1, mpm, m);
      hs_ok += same;
      hs_n++;
      // The MULTI-Module code (last CRC bit from the next byte): same CRC only
      // if that bit equals the last payload bit
      uint8_t g = rand();
      m = hs6200(addrs[a], 5, p, len, pid, false, g, mpm);
      hs_bug_same += !memcmp(cc + 1, mpm, m);
      if (!roundtrip(&f, cc, n, p, len)) { printf("hs6200 roundtrip len %d\n", len); fails++; }
    }
  }
  CHECK(hs_ok == hs_n);
  printf("hs6200: %d/%d bit exact (fixed CRC), MPM code with random next byte: %d/%d\n", hs_ok, hs_n,
         hs_bug_same, hs_n);

  // RF2500: E129 (16/19 bytes, scrambled), E010R5 (14, not scrambled)
  struct { const char* addr; uint8_t len; bool scr; } r[] = {
      {"\xE2\x32\xE0\xC8", 16, true}, {"\xE2\x32\xE0\xC8", 19, true}, {"\x0E\x54\x96\xEE", 14, false}};
  for (int k = 0; k < 3; k++) {
    frame_t f = {.type = FRAME_RF2500, .addr_len = 4, .crc_len = 2, .scramble = r[k].scr};
    memcpy(f.addr, r[k].addr, 4);
    CHECK(frame_init(&f));
    int ok = 0;
    for (int t = 0; t < 200; t++) {
      uint8_t p[32], b[32];
      for (int i = 0; i < r[k].len; i++) b[i] = p[i] = rand();
      uint16_t n = full(&f, p, r[k].len, cc);
      uint8_t m = rf2500((const uint8_t*)r[k].addr, b, r[k].len, r[k].scr, mpm);
      ok += cc[0] == 0xAA && n == m + 1 && !memcmp(cc + 1, mpm, m);
      if (!roundtrip(&f, cc, n, p, r[k].len)) { printf("rf2500 roundtrip\n"); fails++; }
      // 1 chip error per bit is corrected
      uint8_t bad[1200], out[32], out_len;
      memcpy(bad, cc, n);
      for (uint16_t i = 8 * f.hdr_len; i < 8 * n; i += 4) bad[(i + t % 4) >> 3] ^= 0x80 >> ((i + t % 4) & 7);
      if (frame_decode(&f, bad + f.rx_start, n - f.rx_start, r[k].len, out, &out_len) != FRAME_OK ||
          memcmp(out, p, r[k].len)) { printf("rf2500 fec\n"); fails++; }
    }
    printf("rf2500 len %d scramble %d: %d/200 bit exact, rx_sync %08x start %d\n", r[k].len, r[k].scr, ok,
           frame_rx_sync(&f), f.rx_start);
    CHECK(ok == 200);
  }
  CHECK(!frame_len_ok(&(frame_t){.type = FRAME_RF2500, .scramble = true}, 15));
  printf(fails ? "FAILED\n" : "OK\n");
  return fails != 0;
}
