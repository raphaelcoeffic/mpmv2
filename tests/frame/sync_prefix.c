// Host test: early sync alignments and prefixes
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int check(frame_t* f, const uint8_t* pl, uint8_t len)
{
  int fails = 0;
  uint8_t data[64], full[80];
  uint8_t n = frame_tx_data(f, pl, len, data);
  uint32_t sw = frame_tx_sync(f);
  for (int i = 0; i < f->sync_len; i++) full[i] = sw >> (8 * (f->sync_len - 1 - i));
  memcpy(full + f->sync_len, data, n);
  uint8_t total = f->sync_len + n;
  uint8_t rx_len = frame_rx_len(f, len);
  printf("type=%d addr=%02x.. prefix=%d: rx sync %08x, data from byte %d (earliest %d)\n", f->type,
         f->addr[0], f->prefix_len, frame_rx_sync(f), f->rx_start, f->rx_first_start);
  // Receiver synced on each copy of the RX sync word in the frame
  for (int start = f->rx_first_start; start <= f->rx_start; start++) {
    uint32_t w = 0;
    for (int i = start - f->sync_len; i < start; i++) w = (w << 8) | (i < 0 ? 0 : full[i]);
    if (start != f->rx_start && (start < f->sync_len || w != frame_rx_sync(f))) continue;
    uint8_t rx[64] = {0};
    memcpy(rx, full + start, total - start);
    uint8_t out[32], out_len;
    frame_result_t r = frame_decode(f, rx, rx_len, len, out, &out_len);
    if (r != FRAME_OK || out_len != len || memcmp(out, pl, len)) {
      printf("FAIL type=%d start=%d r=%d\n", f->type, start, r);
      fails++;
    }
  }
  // Corrupted payload byte must fail the CRC
  uint8_t rx[64];
  memcpy(rx, full + f->rx_start, total - f->rx_start);
  rx[total - f->rx_start - 3] ^= 0x10;
  uint8_t out[32], out_len;
  if (f->crc_len && frame_decode(f, rx, rx_len, len, out, &out_len) == FRAME_OK) {
    printf("FAIL type=%d corrupted packet accepted\n", f->type);
    fails++;
  }
  return fails;
}

int main()
{
  int fails = 0;
  uint8_t pl[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  frame_t nrf = {.type = FRAME_NRF, .addr = {0xc3, 0xc3, 0xc3, 0xc3, 0xc3}, .addr_len = 5,
                 .crc_len = 2, .sync_bytes = 4};
  frame_init(&nrf);
  fails += check(&nrf, pl, 16);
  nrf.prefix_len = 3; memcpy(nrf.prefix, pl, 3); frame_init(&nrf);
  fails += check(&nrf, pl, 16);
  const uint8_t addrs[][5] = {{0xe7, 0xe7, 0xe7, 0xe7, 0xe7}, {0x55, 0x55, 0x55, 0x55, 0x55},
                              {0xa1, 0xb2, 0xc3, 0xd4, 0xe5}, {0x01, 0x02, 0x03, 0x04, 0x05}};
  for (int i = 0; i < 4; i++) {
    frame_t f = {.type = FRAME_NRF, .addr_len = 5, .crc_len = 2, .sync_bytes = 4};
    memcpy(f.addr, addrs[i], 5);
    frame_init(&f);
    fails += check(&f, pl, 16);
    f.esb = true; f.dpl = true; frame_init(&f);
    fails += check(&f, pl, 16);
  }
  frame_t xn = {.type = FRAME_XN297, .addr = {0x12, 0x34, 0x56, 0x78, 0x9a}, .addr_len = 5,
                .crc_len = 2, .scramble = 1};
  frame_init(&xn);
  fails += check(&xn, pl, 16);
  xn.prefix_len = 2; memcpy(xn.prefix, pl, 2); frame_init(&xn);
  fails += check(&xn, pl, 16);
  xn.prefix_len = 0; xn.enhanced = 1; frame_init(&xn);
  fails += check(&xn, pl, 16);
  printf("%d failures\n", fails);
  return fails != 0;
}
