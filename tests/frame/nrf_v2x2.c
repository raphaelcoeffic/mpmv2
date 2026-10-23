// NRF ShockBurst framing (no packet control field) vs packets captured from a
// WLtoys K989 car transmitter (V2x2, sdr.py v2x2: payload and CRC as received)
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int fails;
static void hex(const char* h, uint8_t* b) { for (int i = 0; h[2*i]; i++) sscanf(h + 2*i, "%2hhx", &b[i]); }

static void check(const char* name, const char* cap)
{
  // V2X2_RF_init(): TX_ADDR 66 88 68 68 68, CRC 16 bits, no auto ack
  frame_t f = {.type = FRAME_NRF, .addr = {0x66, 0x88, 0x68, 0x68, 0x68}, .addr_len = 5,
               .crc_len = 2, .sync_bytes = 4};
  if (!frame_init(&f)) { printf("%s: init failed\n", name); fails++; return; }
  uint8_t c[18], data[64];
  hex(cap, c);
  uint8_t n = frame_tx_data(&f, c, 16, data);
  // On air: the address last byte first, payload, CRC. data: after the TX
  // sync word (4 address bytes)
  static const uint8_t addr[5] = {0x68, 0x68, 0x68, 0x88, 0x66};
  bool ok = n == 19 && data[0] == 0x66 && !memcmp(data + 1, c, 18) && !memcmp(f.known, addr, 5);
  // decode the capture back
  uint8_t full[32], out[32], out_len;
  memcpy(full, addr, 5);
  memcpy(full + 5, c, 18);
  uint8_t rxn = 23 - f.rx_start;
  frame_result_t r = frame_decode(&f, full + f.rx_start, rxn, 16, out, &out_len);
  ok = ok && r == FRAME_OK && out_len == 16 && !memcmp(out, c, 16);
  full[10] ^= 0x01;  // one payload bit: CRC must fail
  ok = ok && frame_decode(&f, full + f.rx_start, rxn, 16, out, &out_len) == FRAME_BAD;
  printf("%-10s %s\n", name, ok ? "ok" : "FAIL");
  if (!ok) {
    fails++;
    for (int i = 0; i < n; i++) printf("%02x", data[i]);
    printf("\n  %s\n", cap);
  }
}

int main()
{
  check("bind", "000000000000007f08c800000000c00f168c");
  check("forward", "00007f003c40007f08c800000000004af80a");
  check("left", "00ff00003c40007f08c80000000000ca703f");
  printf("%d failures\n", fails);
  return fails != 0;
}
