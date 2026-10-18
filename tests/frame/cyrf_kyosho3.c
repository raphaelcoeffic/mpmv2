// CYRF6936 Kyosho3 bind setup: 32 chip SOP (default code 82c79036219eff17),
// 64 chip 8DR data (default codes), length, no CRC, TX_INV: capture of the MPM
// CYRF6936 starting 2 chips before the SOP (after a cccccccc sync on the
// inverted preamble)
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int hex(const char* h, uint8_t* b) { int i; for (i = 0; h[2*i] && h[2*i+1] > ' '; i++) sscanf(h + 2*i, "%2hhx", &b[i]); return i; }
static int bit(const uint8_t* b, int i) { return (b[i >> 3] >> (7 - (i & 7))) & 1; }

int main()
{
  frame_t f = {.type = FRAME_CYRF, .sop = true, .sop32 = true, .var_len = true, .invert = true,
               .sop_code = {0x82, 0xC7, 0x90, 0x36, 0x21, 0x9E, 0xFF, 0x17},
               .data_code = {0x6F, 0xBE, 0x32, 0x01, 0xDB, 0xF1, 0x2B, 0x01, 0xE3, 0x5C, 0xFA, 0x02, 0x97, 0x93, 0xF9, 0x02}};
  CHECK(frame_init(&f));
  uint8_t p[4], cap[64], tx[200], out[4], out_len;
  hex("aa010203", p);
  int clen = hex("ef873da4d5078c25b2cfa9901b71fa901f815bf84c159bf923d9f532036e3f5203b3ea6406dc7ea4", cap);
  uint16_t n = frame_tx_data(&f, p, 4, tx);
  // after the SOP part: gap + 5 symbols of 64 chips; the capture has 40 bytes
  int nbits = clen * 8 - 2, bad = 0;
  for (int i = 0; i < nbits; i++) bad += bit(tx, 68 - 32 + i) != bit(cap, 2 + i);
  bool enc = bad == 0;
  // RX
  uint8_t stream[200];
  memcpy(stream, f.known, f.hdr_len);
  memcpy(stream + f.hdr_len, tx + f.hdr_len - 4, n - (f.hdr_len - 4));
  frame_result_t r = frame_decode(&f, stream + f.rx_start, f.hdr_len + n - (f.hdr_len - 4) - f.rx_start, 4, out, &out_len);
  bool dec = r == FRAME_OK && out_len == 4 && !memcmp(out, p, 4);
  printf("kyosho3 n=%u rx_sync=%08x encode %s (%d bad bits of %d) decode %s\n", n, frame_rx_sync(&f),
         enc ? "ok" : "FAIL", bad, nbits, dec ? "ok" : "FAIL");
  CHECK(enc && dec);
  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
