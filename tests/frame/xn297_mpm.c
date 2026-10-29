// Host test: mpm-v2 frame.c against the MULTI-Module XN297 emulation code
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frame.h"

#define PROGMEM
#define pgm_read_word(x) (*(x))
#define XN297DUMP_NRF24L01_INO
enum { XN297_NRF, XN297_CC2500 };
bool xn297_scramble_enabled, xn297_crc, xn297_bitrate, xn297_rf;
uint8_t xn297_addr_len, xn297_rx_packet_len;
uint8_t xn297_tx_addr[5], xn297_rx_addr[5];
uint16_t crc, crc16_polynomial = 0x1021;

static uint8_t bit_reverse(uint8_t b_in)
{
  uint8_t b_out = 0;
  for (uint8_t i = 0; i < 8; ++i) { b_out = (b_out << 1) | (b_in & 1); b_in >>= 1; }
  return b_out;
}
static void crc16_update(uint8_t a, uint8_t bits)
{
  crc ^= a << 8;
  while (bits--) crc = crc & 0x8000 ? (crc << 1) ^ crc16_polynomial : crc << 1;
}
static uint8_t sent[64], sent_len, rxbuf[64];
static void XN297_SendPayload(uint8_t* msg, uint8_t len) { memcpy(sent, msg, len); sent_len = len; }
static void XN297_ReceivePayload(uint8_t* msg, uint8_t len) { if (xn297_crc) len += 2; memcpy(msg, rxbuf, len); }

#include "mpm/mpm_tables.inc"
#include "mpm/mpm_funcs.inc"

static void dump(const char* n, const uint8_t* b, int l) { printf("%s", n); for (int i = 0; i < l; i++) printf("%02x", b[i]); printf("\n"); }

int main()
{
  int fails = 0, tests = 0;
  srand(1);
  for (int enh = 0; enh <= 1; enh++)
  for (int alen = 3; alen <= 5; alen++)
  for (int scr = 0; scr <= 1; scr++)
  for (int crc_en = 0; crc_en <= 1; crc_en++)
  for (int len = 1; len <= 16; len++) {
    if (enh && !crc_en) continue;  // MPM drops the last byte without CRC
    uint8_t addr[5], pl[32];
    for (int i = 0; i < 5; i++) addr[i] = rand();
    for (int i = 0; i < len; i++) pl[i] = rand();

    // MPM side (CC2500 path: no 0x55 inserted in the payload)
    xn297_rf = XN297_CC2500; xn297_scramble_enabled = scr; xn297_crc = crc_en;
    xn297_addr_len = alen; memcpy(xn297_tx_addr, addr, alen);
    XN297_scramble_init();  // as XN297_SetRXAddr()
    for (int i = alen; i--; ) xn297_rx_addr[i] = addr[i] ^ XN297_scramble_byte();

    // mpm-v2 side
    frame_t f = {.type = FRAME_XN297, .addr_len = alen, .crc_len = crc_en ? 2 : 0,
                 .scramble = scr, .enhanced = enh};
    memcpy(f.addr, addr, alen);
    if (!frame_init(&f)) { printf("init failed\n"); return 1; }
    // Keep the enhanced mode pid in sync with the MPM static counter
    static uint8_t pid;
    f.pid = pid;

    uint8_t data[64], frame[80];
    uint8_t n = frame_tx_data(&f, pl, len, data);
    uint32_t sw = frame_tx_sync(&f);
    frame[0] = sw >> 24; frame[1] = sw >> 16; frame[2] = sw >> 8; frame[3] = sw;
    memcpy(frame + 4, data, n);

    if (enh) XN297_WriteEnhancedPayload(pl, len, 0); else XN297_WritePayload(pl, len);
    pid = (pid + 1) & 3;
    tests++;
    bool tx_same = !enh || scr ? (sent_len == n && !memcmp(sent, data, n)) : true;
    // MPM decoder on our frame (bytes after the address)
    memcpy(rxbuf, frame + 4 + alen, n - alen);
    uint8_t msg[32];
    bool mpm_ok = enh ? XN297_ReadEnhancedPayload(msg, len) == len : XN297_ReadPayload(msg, len);
    mpm_ok = mpm_ok && !memcmp(msg, pl, len);
    // Our decoder on the MPM frame
    uint8_t mframe[80] = {0x0C, 0x71, 0x0F, 0x55};
    memcpy(mframe + 4, sent, sent_len);
    uint8_t out[32], out_len;
    frame_result_t r = frame_decode(&f, sent + f.rx_start - 4, sent_len - (f.rx_start - 4), len, out, &out_len);
    bool our_ok = r == FRAME_OK && out_len == len && !memcmp(out, pl, len);
    if (enh && !scr) our_ok = true;  // MPM TX unscrambled enhanced: no xorout

    if (!tx_same || !mpm_ok || !our_ok) {
      fails++;
      printf("FAIL enh=%d alen=%d scr=%d crc=%d len=%d tx_same=%d mpm_ok=%d our_ok=%d (r=%d)\n",
             enh, alen, scr, crc_en, len, tx_same, mpm_ok, our_ok, r);
      dump("  ours: ", data, n); dump("  mpm:  ", sent, sent_len);
      if (fails > 5) return 1;
    }
  }
  printf("%d tests, %d failures\n", tests, fails);
  return fails != 0;
}
