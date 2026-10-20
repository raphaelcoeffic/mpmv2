// CYRF6936 (DSM data packets) framing vs captures of the MPM CYRF6936: SOP and
// data codes of PN row 0, SOP column 0, CRC seed 0 and 0x1234. The captures
// are the CC2652 bytes after the sync word 3fc289ae (first 32 chips of the
// second, inverted, SOP code: header bytes 17 to 20).
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int hex(const char* h, uint8_t* b) { int i; for (i = 0; h[2*i] && h[2*i+1] > ' '; i++) sscanf(h + 2*i, "%2hhx", &b[i]); return i; }
static uint32_t _sync(const uint8_t* b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static const uint8_t sop[8] = {0x03, 0xBC, 0x6E, 0x8A, 0xEF, 0xBD, 0xFE, 0xF8};
static const uint8_t data[16] = {0x40, 0xBA, 0x97, 0xD5, 0x86, 0x4F, 0xCC, 0xD1,
                                 0xD7, 0xA1, 0x54, 0xB1, 0x5E, 0x89, 0xAE, 0x86};

static void check(const char* name, uint16_t seed, const char* payload_hex, const char* cap_hex)
{
  frame_t f = {.type = FRAME_CYRF, .crc_len = 2, .var_len = true, .crc_seed = seed, .sop = true};
  memcpy(f.sop_code, sop, 8);
  memcpy(f.data_code, data, 16);
  CHECK(frame_init(&f));
  CHECK(f.hdr_len == 25 && _sync(f.known + 17) == 0x3fc289ae);
  uint8_t p[32], cap[256], tx[256], out[32], out_len;
  int len = hex(payload_hex, p), clen = hex(cap_hex, cap);
  uint16_t n = frame_tx_data(&f, p, len, tx);
  // all chips up to the CRC (the last byte holds idle chips)
  bool enc = n - 17 <= clen && !memcmp(tx + 17, cap, n - 17 - 1);
  // RX: the stream after the chosen RX sync word
  uint8_t stream[300];
  memcpy(stream, f.known, 21);
  memcpy(stream + 21, cap, clen);
  uint16_t rxl = frame_rx_len(&f, 16);
  frame_result_t r = frame_decode(&f, stream + f.rx_start, clen + 21 - f.rx_start, 16, out, &out_len);
  bool dec = r == FRAME_OK && out_len == len && !memcmp(out, p, len);
  // 8 chip errors in every symbol: still decoded (CYRF DATA64_THOLD 10)
  for (int s = 0; s < len + 3; s++)
    for (int k = 0; k < 8; k++) {
      int bit = 8 * 25 + 1 + s * 64 + k * 7;
      stream[bit / 8] ^= 0x80 >> (bit % 8);
    }
  bool robust = frame_decode(&f, stream + f.rx_start, clen + 21 - f.rx_start, 16, out, &out_len) == FRAME_OK &&
                out_len == len && !memcmp(out, p, len);
  printf("%-10s n=%u rx_len=%u rx_start=%u encode %s decode %s 8-errors %s\n", name, n, rxl, f.rx_start,
         enc ? "ok" : "FAIL", dec ? "ok" : "FAIL", robust ? "ok" : "FAIL");
  CHECK(enc && dec && robust);
}

// One RX (64 chip codes and the 32 chip alternative, frame_cyrf_rx_alt())
// for packets sent with either code length: each decodes with its own frame
static void check_alt(void)
{
  frame_t f = {.type = FRAME_CYRF, .crc_len = 2, .var_len = true, .crc_seed = 0x1234, .sop = true}, alt;
  memcpy(f.sop_code, sop, 8);
  memcpy(f.data_code, data, 16);
  CHECK(frame_init(&f) && frame_cyrf_rx_alt(&f, &alt));
  CHECK(alt.data32 && f.rx_sync == alt.rx_sync && f.rx_start == alt.rx_start);
  for (int d32 = 0; d32 < 2; d32++) {
    frame_t t = f;
    t.data32 = d32;
    CHECK(frame_init(&t));
    uint8_t p[16] = {0x7f, 0, 0, 0x64, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, full[300], out[32], out_len;
    memcpy(full, t.known, t.sync_len);
    uint16_t n = t.sync_len + frame_tx_data(&t, p, sizeof(p), full + t.sync_len);
    CHECK(_sync(full + f.rx_start - f.sync_len) == f.rx_sync);
    CHECK(frame_cyrf_rx_is_alt(&f, &alt, full + f.rx_start, n - f.rx_start) == d32);
    // Variable length RX: the length symbol of either, after the longer header
    uint8_t hdr = frame_rx_hdr_len(&f) > frame_rx_hdr_len(&alt) ? frame_rx_hdr_len(&f) : frame_rx_hdr_len(&alt);
    uint16_t rx = frame_rx_var_len(d32 ? &alt : &f, 16, full + f.rx_start, hdr);
    CHECK(hdr == 17 && rx == n - f.rx_start);
    frame_result_t r64 = frame_decode(&f, full + f.rx_start, n - f.rx_start, 16, out, &out_len);
    frame_result_t r32 = frame_decode(&alt, full + f.rx_start, n - f.rx_start, 16, out, &out_len);
    bool ok = (d32 ? r32 : r64) == FRAME_OK && (d32 ? r64 : r32) != FRAME_OK && out_len == 16 &&
              !memcmp(out, p, 16);
    printf("alt %s chips: decode %s\n", d32 ? "32" : "64", ok ? "ok" : "FAIL");
    CHECK(ok);
  }
}

int main()
{
  check_alt();
  check("seed0", 0, "000102030405060708090a0b0c0d0e0f", "084280e08b2a4f06e63a7ed17ed10b2a4f06e63a7da216549e0dcc74fb442ca93c1b98e9f6885952783731d3ed10b2a4f06e63a7da216549e0dcc74fb442ca93c1b98e9fe885952783731d3f510b2a4f06e63a7ea216549e0dcc74fdc42ca93c1b98e9fb085952783731d3f690b2a4f06e63a7ed216549e0dcc74fda42ca93c1b98e9fb405952783731d3f68b0f5c29546bd48badcc74fda216549e0e1c76ae7feb7bedf8d");
  check("seed1234", 0x1234, "55aa00ff", "084280e0ed10b2a4f06e63a7d72856e8a9e147ad64671604bbd356c3fed10b2a4f06e63a7ae14aa35ea45d586ad87c8ce2c0977a216549e0dcc74fda61dfbdfcf2b75bbebf7efefffff73f7b979bbfffdffff7efefe6e57fedab7d6f77ffd7f7cfbfb57795fdf7fffcfedfdf4f7cace9cff7a77fff4e7fdffd3eeefadfec65b7853fef78fde27fc75faeb5fc1f7f97dd6b9d53d73febfa377ffdfbdec9fddbecbfff7ff52e");
  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
