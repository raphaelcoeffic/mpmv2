// FrSky L framing vs the MULTI-Module FrSkyL_encode_packet() / FRSKYL_init()
// buffer (256 bytes sent by the CC2500)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint8_t buffer[256], packet[17];
static void encode(int type)
{
  uint32_t bits = 0;
  uint8_t bitsavailable = 0, idx = 0, len = 6;
  if (type) { idx = 66; len = 17; }
  for (uint8_t i = 0; i < len; i++) {
    uint8_t tmp = packet[i];
    for (uint8_t j = 0; j < 8; j++) {
      bits <<= 11;
      bits |= tmp & 1 ? 0x712 : 0xED;
      tmp >>= 1;
      bitsavailable += 11;
      while (bitsavailable >= 8) {
        buffer[idx++] = bits >> (bitsavailable - 8);
        bitsavailable -= 8;
      }
    }
  }
}

int main(int argc, char** argv)
{
  srand(1);
  memset(buffer, 0, 253);
  memset(buffer + 253, 0x55, 3);
  memset(packet, 0xAA, 6);
  encode(0);
  for (int pre = 0; pre < 2; pre++) {
    frame_t f = {.type = FRAME_FRSKYL, .prefix = {0x4E}, .prefix_len = pre};
    CHECK(frame_init(&f));
    printf("prefix %d: hdr %d known %d tx_sync %08x rx_sync %08x start %d first %d errors %d weak %d\n", pre,
           f.hdr_len, f.known_len, frame_tx_sync(&f), frame_rx_sync(&f), f.rx_start, f.rx_first_start,
           f.rx_sync_errors, frame_rx_sync_weak(&f));
    int ok = 0, dec = 0;
    for (int t = 0; t < 200; t++) {
      for (int i = 0; i < 17; i++) packet[i] = rand();
      packet[0] = 0x4E;
      encode(1);
      uint8_t air[1200], out[32], out_len;
      uint32_t sw = frame_tx_sync(&f);
      for (int i = 0; i < 4; i++) air[i] = sw >> (24 - 8 * i);
      uint16_t n = 4 + frame_tx_data(&f, packet, 17, air + 4);
      ok += n == 256 && !memcmp(air, buffer, 256);
      // 5 chip errors per bit (11 chips) corrected
      for (int b = 0; b < 17 * 8; b++)
        for (int k = 0; k < 5; k++) {
          int c = 8 * 66 + b * 11 + (k * 2 + t) % 11;
          if (pre && c < 8 * f.known_len) continue;  // known bytes are compared as sent
          air[c >> 3] ^= 0x80 >> (c & 7);
        }
      dec += frame_decode(&f, air + f.rx_start, n - f.rx_start, 17, out, &out_len) == FRAME_OK &&
             out_len == 17 && !memcmp(out, packet, 17);
    }
    printf("  %d/200 bit exact, %d/200 decoded with chip errors, rx_len %d\n", ok, dec, frame_rx_len(&f, 17));
    CHECK(ok == 200 && dec == 200);
  }
  printf(fails ? "FAILED\n" : "OK\n");
  return fails != 0;
}
