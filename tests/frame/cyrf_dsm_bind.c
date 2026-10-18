// CYRF6936 SDR (DSM bind packets: no SOP, length or CRC, data code of PN row 0
// column 8) vs a capture of the MPM CYRF6936: CC2652 bytes after the sync word
// cccccccc (last 32 preamble chips). The capture holds the first 3 bytes.
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int hex(const char* h, uint8_t* b) { int i; for (i = 0; h[2*i] && h[2*i+1] > ' '; i++) sscanf(h + 2*i, "%2hhx", &b[i]); return i; }

int main()
{
  frame_t f = {.type = FRAME_CYRF, .sdr = true,
               .data_code = {0xD7, 0xA1, 0x54, 0xB1, 0x5E, 0x89, 0xAE, 0x86}};
  CHECK(frame_init(&f));
  CHECK(f.hdr_len == 8 && f.known[0] == 0xCC && f.known[7] == 0xCC);
  uint8_t full[16], cap[256], tx[1100], out[16], out_len;
  hex("a5f00f5a0123456789abcdef55aa00ff", full);
  int clen = hex("f5c29546bd48bab08a3d6ab942b7454f75c29546bd48bab08a3d6ab942b7454f0a3d6ab942b7454f75c29546bd48bab08a3d6ab942b7454f75c29546bd48bab08a3d6ab942b7454f0a3d6ab942b7454f0a3d6ab942b7454f0a3d6ab942b7454f75c29546bd48bab0f5c29546bd48bab0f5c29546bd48bab0f5c29546bd48bab0f5c29546bd48bab0f5c29546bd48bab0f5c29546bd48bab0f5c29546bd48bab08a3d6ab942b7454f0a3d6ab942b7454f0a3d6ab942b7454f0a3d6ab942b7454f0a3d6ab942b7454f", cap);
  uint16_t n = frame_tx_data(&f, full, 16, tx);
  CHECK(n == 8 + 1025 - 4);
  // tx: data after the TX sync (preamble bytes 0-3), the capture starts at header byte 8
  bool enc = !memcmp(tx + 4, cap, 192);
  uint8_t stream[300];
  memcpy(stream, f.known, 8);
  memcpy(stream + 8, cap, clen);
  frame_result_t r = frame_decode(&f, stream + f.rx_start, 8 + clen - f.rx_start, 3, out, &out_len);
  bool dec = r == FRAME_OK && out_len == 3 && !memcmp(out, full, 3);
  // 20 chip errors per bit still decode
  for (int bit = 0; bit < 24; bit++)
    for (int k = 0; k < 20; k++) {
      int c = 64 + 1 + bit * 64 + k * 3;
      stream[c / 8] ^= 0x80 >> (c % 8);
    }
  bool robust = frame_decode(&f, stream + f.rx_start, 8 + clen - f.rx_start, 3, out, &out_len) == FRAME_OK &&
                !memcmp(out, full, 3);
  // RX with a known prefix (bind response: the first bytes are known): sync in the data
  frame_t g = f;
  memcpy(g.prefix, full, 2);
  g.prefix_len = 2;
  CHECK(frame_init(&g));
  memcpy(stream + 8, cap, clen);
  bool pre = g.known_len > 8 && frame_decode(&g, stream + g.rx_start, 8 + clen - g.rx_start, 3, out, &out_len) == FRAME_OK;
  printf("sdr n=%u rx_start=%u encode %s decode %s 20-errors %s; prefix: known=%u rx_start=%u %s\n", n,
         f.rx_start, enc ? "ok" : "FAIL", dec ? "ok" : "FAIL", robust ? "ok" : "FAIL", g.known_len,
         g.rx_start, pre ? "ok" : "FAIL");
  CHECK(enc && dec && robust && pre);
  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
