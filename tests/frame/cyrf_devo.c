// CYRF6936 32 chip SOP and data codes (Walkera Devo bind setup: SOP code
// DEVO_j6pro_sopcodes[0], default data codes, CRC seed 0, length) vs a capture
// of the MPM CYRF6936 starting at the SOP code
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int hex(const char* h, uint8_t* b) { int i; for (i = 0; h[2*i] && h[2*i+1] > ' '; i++) sscanf(h + 2*i, "%2hhx", &b[i]); return i; }
static int bit(const uint8_t* b, int i) { return (b[i >> 3] >> (7 - (i & 7))) & 1; }

int main()
{
  frame_t f = {.type = FRAME_CYRF, .sop = true, .sop32 = true, .data32 = true, .crc_len = 2, .var_len = true,
               .sop_code = {0x3C, 0x37, 0xCC, 0x91, 0xE2, 0xF8, 0xCC, 0x91},
               .data_code = {0x6F, 0xBE, 0x32, 0x01, 0xDB, 0xF1, 0x2B, 0x01, 0xE3, 0x5C, 0xFA, 0x02, 0x97, 0x93, 0xF9, 0x02}};
  CHECK(frame_init(&f));
  uint8_t p[16], cap[256], tx[1100], out[16], out_len;
  hex("000102040810204080033f7fc0ff55aa", p);
  int clen = hex("3cec338953cec338959bf84c104c159bf8982b37f130566fe4c159bf84159bf84d9bf84c1123815bf9c62d05ffb3ea640260acdfc491c0adfc58d982fe39d2fa03a7267d00bf38c5a1fa901b711c0adfcb05f8b1b63fd5ffd8b9ebef57deffa5eebef5fb", cap);
  uint16_t n = frame_tx_data(&f, p, 16, tx);
  // header 17 bytes (68 preamble chips, SOP, 0101, SOP), 19 symbols of 32 chips + gap
  CHECK(f.hdr_len == 17 && n == 17 - 4 + (1 + 19 * 32 + 7) / 8);
  // tx starts after the 32 bit TX sync, the capture at the SOP (chip 68)
  int nbits = (1 + 19 * 32) + 68, bad = 0;
  for (int i = 0; i < nbits; i++) if (bit(tx, 68 - 32 + i) != bit(cap, i)) { bad++; printf("bit %d of %d\n", i, nbits); }
  bool enc = bad == 0;
  // RX: from the frame start
  uint8_t stream[300];
  memcpy(stream, f.known, f.hdr_len);
  for (int i = 0; i < clen * 8 && 68 + i < (int)sizeof(stream) * 8; i++) {
    int j = 68 + i;
    if (bit(cap, i)) stream[j >> 3] |= 0x80 >> (j & 7); else stream[j >> 3] &= ~(0x80 >> (j & 7));
  }
  uint16_t avail = 17 + (1 + 19 * 32 + 7) / 8;
  frame_result_t r = frame_decode(&f, stream + f.rx_start, avail - f.rx_start, 16, out, &out_len);
  bool dec = r == FRAME_OK && out_len == 16 && !memcmp(out, p, 16);
  // 4 chip errors per 32 chip symbol still decode
  for (int s = 0; s < 19; s++)
    for (int k = 0; k < 4; k++) {
      int c = 8 * 17 + 1 + s * 32 + k * 7;
      stream[c / 8] ^= 0x80 >> (c % 8);
    }
  bool robust = frame_decode(&f, stream + f.rx_start, avail - f.rx_start, 16, out, &out_len) == FRAME_OK &&
                !memcmp(out, p, 16);
  printf("devo n=%u rx_start=%u rx_sync=%08x encode %s (%d bad bits) decode %s 4-errors %s\n", n, f.rx_start,
         frame_rx_sync(&f), enc ? "ok" : "FAIL", bad, dec ? "ok" : "FAIL", robust ? "ok" : "FAIL");
  CHECK(enc && dec && robust);
  // 64 chip SOP / data combinations are accepted, SDR with 32 chips is not
  frame_t g = f; g.sdr = true; g.sop = false;
  CHECK(!frame_init(&g));
  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
