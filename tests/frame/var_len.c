// Variable length RX: frame_rx_hdr_len() / frame_rx_var_len() against the
// packets frame_tx_data() makes, received from each sync word position
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void check(const char* name, frame_t f, uint8_t max)
{
  CHECK(frame_init(&f));
  uint8_t hdr = frame_rx_hdr_len(&f);
  uint16_t max_rx = frame_rx_len(&f, max);
  CHECK(hdr > 0 && hdr <= 32);
  printf("%-14s hdr=%u rx_start=%u first=%u max_rx=%u:", name, hdr, f.rx_start, f.rx_first_start,
         max_rx);
  for (uint8_t len = 1; len <= max; len++) {
    uint8_t p[64], data[200], full[240], out[64], out_len;
    for (int i = 0; i < len; i++) p[i] = i * 37 + len;
    if (f.type == FRAME_CC2500) p[0] = len - 1;  // the length byte
    uint16_t n = frame_tx_data(&f, p, len, data);
    memcpy(full, f.known, f.sync_len);
    memcpy(full + f.sync_len, data, n);
    memset(full + f.sync_len + n, 0x5a, sizeof(full) - f.sync_len - n);
    uint16_t air = f.sync_len + n;  // frame bytes

    // The RF core synced at rx_start, or at an earlier copy of the sync word
    for (int start = f.rx_start; start >= f.rx_first_start; start -= f.rx_start - f.rx_first_start) {
      uint16_t rx = frame_rx_var_len(&f, max, full + start, hdr);
      CHECK(rx >= air - start && rx <= max_rx);
      CHECK(frame_decode(&f, full + start, rx, max, out, &out_len) == FRAME_OK);
      CHECK(out_len == len && !memcmp(out, p, len));
      if (len == 1 || len == max) printf(" %u:%u", len, rx);
      if (start == f.rx_first_start) break;
    }
  }
  printf("\n");

  // Lengths above the maximum: not valid
  uint8_t p[64] = {max}, data[200], full[240];
  uint16_t n = frame_tx_data(&f, p, max + 1, data);
  if (n) {
    memcpy(full, f.known, f.sync_len);
    memcpy(full + f.sync_len, data, n);
    CHECK(frame_rx_var_len(&f, max, full + f.rx_start, hdr) == 0);
  }
}

int main()
{
  const uint8_t addr[5] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5};
  frame_t esb = {.type = FRAME_NRF, .addr_len = 5, .crc_len = 2, .sync_bytes = 4, .esb = true, .dpl = true};
  memcpy(esb.addr, addr, 5);
  check("esb dpl", esb, 32);
  esb.crc_len = 1;
  check("esb dpl crc1", esb, 32);
  // Repeated patterns: the sync word further in, 3 header bytes
  esb.crc_len = 2;
  memset(esb.addr, 0x65, 5);
  check("esb dpl 65..", esb, 32);
  memset(esb.addr, 0xaa, 5);
  check("esb dpl aa..", esb, 32);
  memset(esb.addr, 0x55, 5);
  check("esb dpl 55..", esb, 32);

  frame_t xn = {.type = FRAME_XN297, .addr = {0x65, 0x45, 0xa5, 0xe7, 0xc3}, .addr_len = 5,
                .crc_len = 2, .scramble = true, .enhanced = true};
  check("xn297 enh", xn, 30);
  xn.scramble = false;
  check("xn297 enh unscr", xn, 30);

  frame_t hs = {.type = FRAME_HS6200, .addr_len = 5, .crc_len = 2, .scramble = true, .noack = true};
  memcpy(hs.addr, addr, 5);
  check("hs6200", hs, 15);

  frame_t cc = {.type = FRAME_CC2500, .addr = {0xd3, 0x91}, .addr_len = 2, .crc_len = 2, .var_len = true};
  check("cc2500", cc, 40);
  cc.whitening = true;
  check("cc2500 white", cc, 40);
  cc.fec = true;
  check("cc2500 fec", cc, 40);
  cc.invert = true;
  check("cc2500 fec msk", cc, 40);
  frame_t ccx = {.type = FRAME_CC2500, .addr = {0xd3, 0x91, 0xd3, 0x91}, .addr_len = 4, .crc_len = 2,
                 .var_len = true};
  check("cc2500 sync x2", ccx, 30);

  // CYRF6936 8DR with SOP (DSM): the length is the first data symbol
  frame_t cyrf = {.type = FRAME_CYRF, .crc_len = 2, .var_len = true, .crc_seed = 0x1234, .sop = true,
                  .sop_code = {0x03, 0xBC, 0x6E, 0x8A, 0xEF, 0xBD, 0xFE, 0xF8},
                  .data_code = {0x40, 0xBA, 0x97, 0xD5, 0x86, 0x4F, 0xCC, 0xD1, 0xD7, 0xA1, 0x54,
                                0xB1, 0x5E, 0x89, 0xAE, 0x86}};
  check("cyrf 64", cyrf, 16);
  cyrf.data32 = true;
  check("cyrf 32", cyrf, 16);
  cyrf.invert = true;
  check("cyrf 32 inv", cyrf, 16);

  // Fixed length: none
  frame_t fixed = {.type = FRAME_CC2500, .addr = {0xd3, 0x91}, .addr_len = 2, .crc_len = 2};
  CHECK(frame_init(&fixed) && frame_rx_hdr_len(&fixed) == 0);
  frame_t nrf = {.type = FRAME_NRF, .addr_len = 5, .crc_len = 2, .sync_bytes = 4};
  memcpy(nrf.addr, addr, 5);
  CHECK(frame_init(&nrf) && frame_rx_hdr_len(&nrf) == 0);
  // CYRF6936 SDR (512 chip length), no SOP (not valid)
  frame_t sdr = cyrf;
  sdr.sdr = true;
  sdr.data32 = false;
  CHECK(frame_init(&sdr) && frame_rx_hdr_len(&sdr) == 0);
  frame_t nosop = cyrf;
  nosop.sop = false;
  CHECK(!frame_init(&nosop) || frame_rx_hdr_len(&nosop) == 0);

  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
