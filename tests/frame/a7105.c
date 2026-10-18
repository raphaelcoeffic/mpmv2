// A7105 framing vs captures of the MPM A7105 (FlySky: CRC, AFHDS2A: CRC + FEC)
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int hex(const char* h, uint8_t* b) { int i; for (i = 0; h[2*i]; i++) sscanf(h + 2*i, "%2hhx", &b[i]); return i; }

static void check(const char* name, bool fec, const char* payload_hex, const char* cap_hex)
{
  frame_t f = {.type = FRAME_A7105, .addr = {0x54, 0x75, 0xc5, 0x2a}, .addr_len = 4, .crc_len = 2, .fec = fec};
  CHECK(frame_init(&f));
  uint8_t p[64], cap[128], data[160], out[64], out_len;
  int len = hex(payload_hex, p), clen = hex(cap_hex, cap);
  uint8_t n = frame_tx_data(&f, p, len, data);  // data after the 4 byte ID
  bool enc = n <= clen && !memcmp(data, cap, n);
  frame_result_t r = frame_decode(&f, cap, clen, len, out, &out_len);
  bool dec = r == FRAME_OK && out_len == len && !memcmp(out, p, len);
  // one bit error per code word, on every other word
  int fixed = -1;
  if (fec) {
    for (int i = 0; i < 2 * (len + 2); i += 2) cap[(i * 7 + 3) / 8] ^= 0x80 >> ((i * 7 + 3) % 8);
    fixed = frame_decode(&f, cap, clen, len, out, &out_len) == FRAME_OK && !memcmp(out, p, len);
  }
  printf("%-8s n=%u encode %s decode %s%s\n", name, n, enc ? "ok" : "FAIL", dec ? "ok" : "FAIL",
         fixed < 0 ? "" : fixed ? " fec-correct ok" : " fec-correct FAIL");
  CHECK(enc && dec && fixed != 0);
}

int main()
{
  check("flysky", false, "000102030405060708090a0b0c0d0e0f1011121314", "000102030405060708090a0b0c0d0e0f10111213142c85");
  check("flysky0", false, "000000000000000000000000000000000000000000", "000000000000000000000000000000000000000000c656");
  char p[80] = "";
  for (int i = 0; i < 38; i++) sprintf(p + 2 * i, "%02x", i);
  check("afhds2a", true, p, "000000b005801d009c02c00c403a011404e014c058018806901d007f160058b165859d169c5ac16c45ba17145ce174c5d817885e917d05ff2c00b0b2c58b1d2c9cb2cd388ed3ffdf62041dc16336cecf0bf84a9d27efffeff55b");
  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
