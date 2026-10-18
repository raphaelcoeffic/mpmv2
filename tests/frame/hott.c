// CC2500 HoTT framing (MSK, whitening, FEC) vs a capture of the MPM CC2500
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void hex(const char* h, uint8_t* b) { for (int i = 0; h[2*i]; i++) sscanf(h + 2*i, "%2hhx", &b[i]); }

int main()
{
  // MPM: HOTT_init_values, tx 000102..31; CC2652 raw capture after sync 2c6e2c6e
  uint8_t cap[108];
  hex("e5d615d4f4f76b57cef1d6d16d3403176ad1f18e90cef57a7e096774ff8e07e8d0598301b5e497d80ece48653dc76a7cc6d5c427e0c999a8e53071d1b0763ec5acd0c3bad2936d57f61ba2c790ffcd6dd96321b318769aca5e2e3be5b5bb4335de611f8b983e1236bbcfb810", cap);
  frame_t f = {.type = FRAME_CC2500, .addr = {0xd3, 0x91, 0xd3, 0x91}, .addr_len = 4, .crc_len = 2,
               .whitening = true, .fec = true, .invert = true};
  CHECK(frame_init(&f));
  uint8_t payload[50], data[160], out[64], out_len;
  for (int i = 0; i < 50; i++) payload[i] = i;
  uint8_t n = frame_tx_data(&f, payload, 50, data);
  printf("tx sync %08x n=%u rx_sync %08x rx_start %u rx_len %u\n", frame_tx_sync(&f), n,
         frame_rx_sync(&f), f.rx_start, frame_rx_len(&f, 50));
  CHECK(frame_tx_sync(&f) == 0x55552c6e && n == 110 && data[0] == 0x2c && data[1] == 0x6e);
  CHECK(!memcmp(data + 2, cap, 108));

  // decode the capture, from the RX sync position
  uint8_t full[160];
  memcpy(full, f.known, 6);
  memcpy(full + 6, cap, 108);
  uint8_t rx_len = frame_rx_len(&f, 50);
  CHECK(frame_decode(&f, full + f.rx_start, rx_len, 50, out, &out_len) == FRAME_OK);
  CHECK(out_len == 50 && !memcmp(out, payload, 50));

  // random bit errors: the Viterbi decoder corrects isolated ones
  srand(1);
  int ok1 = 0, ok4 = 0;
  for (int t = 0; t < 200; t++) {
    uint8_t c[160];
    memcpy(c, full, sizeof(full));
    int nerr = t < 100 ? 1 : 4;
    for (int e = 0; e < nerr; e++) {
      int bit = 6 * 8 + (t * 37 + e * 211 + rand()) % (108 * 8);
      c[bit / 8] ^= 0x80 >> (bit % 8);
    }
    frame_result_t r = frame_decode(&f, c + f.rx_start, rx_len, 50, out, &out_len);
    bool good = r == FRAME_OK && !memcmp(out, payload, 50);
    if (t < 100) ok1 += good; else ok4 += good;
  }
  printf("1 bit error: %d/100 corrected, 4 spread errors: %d/100\n", ok1, ok4);
  CHECK(ok1 == 100);

  // whitening without FEC, variable length (Hitec, RadioLink): round trip
  frame_t g = {.type = FRAME_CC2500, .addr = {0xab, 0xcd, 0xab, 0xcd}, .addr_len = 4, .crc_len = 2,
               .var_len = true, .whitening = true};
  CHECK(frame_init(&g));
  uint8_t p2[34] = {0x21};
  for (int i = 1; i < 34; i++) p2[i] = i * 3;
  n = frame_tx_data(&g, p2, 34, data);
  memcpy(full, g.known, 4 + 2);
  memcpy(full + g.sync_len, data, n);
  CHECK(frame_decode(&g, full + g.rx_start, frame_rx_len(&g, 40), 40, out, &out_len) == FRAME_OK &&
        out_len == 34 && !memcmp(out, p2, 34));
  // IKEA: FEC with a variable length, captured from the CC2500 (noise after it)
  {
    frame_t k = {.type = FRAME_CC2500, .addr = {0xd3, 0x91, 0xd3, 0x91}, .addr_len = 4, .crc_len = 2,
                 .var_len = true, .fec = true, .invert = true};
    CHECK(frame_init(&k));
    uint8_t ik[7] = {0x06, 0x55, 0x01, 0x12, 0x34, 0x03, 0xaa}, air[48];
    hex("f767f37f83cc0cad7134f0c4258cc60eb2cc8320ccd5ac77e15003ff8b1f4b24c5f8c3e0a1b2c3d4e5f6", air);
    n = frame_tx_data(&k, ik, 7, data);
    CHECK(n == 22 && !memcmp(data + 2, air, 20));
    memcpy(full, k.known, 6);
    memcpy(full + 6, air, 42);
    CHECK(frame_decode(&k, full + k.rx_start, frame_rx_len(&k, 16), 16, out, &out_len) == FRAME_OK &&
          out_len == 7 && !memcmp(out, ik, 7));
  }
  // SFHSS: CC2400_EN CRC, captured from the CC2500
  {
    frame_t k = {.type = FRAME_CC2500, .addr = {0xd3, 0x91, 0xd3, 0x91}, .addr_len = 4,
                 .crc_len = 2, .cc2400_crc = true};
    CHECK(frame_init(&k));
    const char* caps[][2] = {{"81000102030405060708090a0b", "b3ec"}, {"000000000000000000000000ff", "00ff"},
                             {"0102030405060708090a0b0c0d", "1b6d"}, {"00000000000000000000000000", "0000"}};
    for (int i = 0; i < 4; i++) {
      uint8_t pl[13], c[2];
      hex(caps[i][0], pl); hex(caps[i][1], c);
      n = frame_tx_data(&k, pl, 13, data);
      CHECK(n == 17 && data[15] == c[0] && data[16] == c[1]);
    }
  }
  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
