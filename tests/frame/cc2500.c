// CC2500 framing: CRC (CRC-16/CMS check value), round trip, sync choice
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void roundtrip(const char* name, frame_t f, const uint8_t* p, uint8_t len, uint8_t max)
{
  CHECK(frame_init(&f));
  uint8_t data[64], full[80], out[64], out_len;
  uint8_t n = frame_tx_data(&f, p, len, data);
  // on air: known (sync part) + data, then padding up to the RX length
  memcpy(full, f.known, f.sync_len);
  memcpy(full + f.sync_len, data, n);
  uint8_t rx_len = frame_rx_len(&f, max);
  memset(full + f.sync_len + n, 0x5a, sizeof(full) - f.sync_len - n);
  printf("%-8s sync=%08x rx_start=%u errors=%u rx_len=%u air=", name, f.rx_sync, f.rx_start,
         f.rx_sync_errors, rx_len);
  for (int i = 0; i < f.sync_len + n; i++) printf("%02x", full[i]);
  printf("\n");
  frame_result_t r = frame_decode(&f, full + f.rx_start, rx_len, max, out, &out_len);
  CHECK(r == FRAME_OK && out_len == len && !memcmp(out, p, len));
  if (f.crc_len) {
    full[f.rx_start + 3] ^= 0x04;
    CHECK(frame_decode(&f, full + f.rx_start, rx_len, max, out, &out_len) == FRAME_BAD);
  }
}

int main()
{
  // CRC-16/CMS check: "123456789" -> 0xaee7, as the CC2500 computes it
  frame_t f = {.type = FRAME_CC2500, .addr = {0xd3, 0x91}, .addr_len = 2, .crc_len = 2};
  CHECK(frame_init(&f));
  uint8_t data[64];
  uint8_t n = frame_tx_data(&f, (const uint8_t*)"123456789", 9, data);
  // TX sync = aaaad391: data = payload + crc
  CHECK(frame_tx_sync(&f) == 0xaaaad391 && frame_sync_bits(&f) == 32);
  CHECK(n == 11 && data[9] == 0xae && data[10] == 0xe7);

  uint8_t frsky[18] = {0x11, 0x03, 0x01, 0xd7, 0x2d, 0x00, 0x00, 0x1e, 0x3c,
                       0x5b, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
  roundtrip("frskyd", (frame_t){.type = FRAME_CC2500, .addr = {0xd3, 0x91}, .addr_len = 2,
                                .crc_len = 2, .var_len = true}, frsky, 18, 18);
  // shorter packet received with the maximum length
  uint8_t short_pkt[5] = {0x04, 1, 2, 3, 4};
  roundtrip("var", (frame_t){.type = FRAME_CC2500, .addr = {0xd3, 0x91}, .addr_len = 2,
                             .crc_len = 2, .var_len = true}, short_pkt, 5, 30);
  // too long length byte
  {
    frame_t g = {.type = FRAME_CC2500, .addr = {0xd3, 0x91}, .addr_len = 2, .crc_len = 2, .var_len = true};
    CHECK(frame_init(&g));
    uint8_t rx[32] = {0x40}, out[64], out_len;
    CHECK(frame_decode(&g, rx, frame_rx_len(&g, 20), 20, out, &out_len) == FRAME_BAD);
  }
  uint8_t x[30];
  for (int i = 0; i < 30; i++) x[i] = i * 7;
  x[0] = 0x1d;
  roundtrip("frskyx", (frame_t){.type = FRAME_CC2500, .addr = {0xd3, 0x91, 0xd3, 0x91},
                                .addr_len = 4, .var_len = true}, x, 30, 30);
  roundtrip("fixed", (frame_t){.type = FRAME_CC2500, .addr = {0x7d, 0x7d}, .addr_len = 2,
                               .crc_len = 2}, x, 20, 20);
  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
