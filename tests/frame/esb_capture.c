// ESB framing vs packets captured from a real nRF24L01+ (MPM bench, 250k)
#include <stdio.h>
#include <string.h>
#include "frame.h"

static int fails;
static void hex(const char* h, uint8_t* b) { for (int i = 0; h[2*i]; i++) sscanf(h + 2*i, "%2hhx", &b[i]); }

static void check(const char* name, bool dpl, bool noack, uint8_t pid, const char* cap)
{
  frame_t f = {.type = FRAME_NRF, .addr = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5}, .addr_len = 5,
               .crc_len = 2, .sync_bytes = 4, .esb = true, .dpl = dpl, .noack = noack, .pid = pid};
  if (!frame_init(&f)) { printf("%s: init failed\n", name); fails++; return; }
  uint8_t payload[16], data[64], c[32];
  for (int i = 0; i < 16; i++) payload[i] = i * 0x11;
  hex(cap, c);
  uint8_t n = frame_tx_data(&f, payload, 16, data);
  // data starts with the last address byte, the capture right after it
  bool ok = n == 21 && data[0] == 0xa1 && !memcmp(data + 1, c, 19) && (data[20] & 0x80) == (c[19] & 0x80);
  // decode the capture back (sync on the last 4 address bytes)
  uint8_t full[64], rx[64], out[32], out_len; uint16_t pcf;
  memcpy(full, f.known, 5); memcpy(full + 5, c, 24);
  uint8_t rxn = 29 - f.rx_start;
  memcpy(rx, full + f.rx_start, rxn);
  frame_result_t r = frame_decode_pcf(&f, rx, rxn, 16, out, &out_len, &pcf);
  ok = ok && r == FRAME_OK && out_len == 16 && !memcmp(out, payload, 16) && ((pcf >> 1) & 3) == pid;
  // corrupt one payload bit: CRC must fail
  rx[rxn - 10] ^= 0x10;
  ok = ok && frame_decode_pcf(&f, rx, rxn, 16, out, &out_len, &pcf) == FRAME_BAD;
  printf("%-10s %s\n", name, ok ? "ok" : "FAIL");
  if (!ok) {
    fails++;
    for (int i = 0; i < n; i++) printf("%02x", data[i]);
    printf("\n  %s\n", cap);
  }
}

int main()
{
  check("static_aa", false, false, 0, "cc80089119a22ab33bc44cd55de66ef77fd4f1b555eba7db");
  check("static_aa1", false, false, 1, "cd80089119a22ab33bc44cd55de66ef77fc413ad77f75e3d");
  check("dpl_aa", true, false, 3, "4380089119a22ab33bc44cd55de66ef77fd147ad5bcbebfd");
  check("dpl_noaa", true, true, 2, "4200089119a22ab33bc44cd55de66ef77fc9d4b9f37bbddb");
  check("dpl_noaa0", true, true, 0, "4000089119a22ab33bc44cd55de66ef77fe810bb5576576f");
  printf("%d failures\n", fails);
  return fails != 0;
}
