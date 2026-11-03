// FASST 10ch framing vs the TM-10 (despread from the R6008HS's DOUT): live
// channels 200 + 170 i; an F/S cycle with F/S on ch1..10 (positions
// 1900 - 150 i), one with ch9 / ch10 on hold (positions 1500 - 150 i);
// normal packets with ch1 / ch2 and with ch9 / ch10
#include <stdlib.h>
#include <string.h>
#include "fasst_frame.h"
#include "test.h"

typedef struct {
  uint8_t k;
  const char* hex;
} ref_t;

// Consecutive frames, F/S types 1..6, the first with ch1 / ch2 in group 1
static const ref_t fs_all[6] = {
    {17, "12a8d30dd78093e1066b12378b1c3d24cd7118828fcf"},
    {7, "12a8d30dda80e7a87c6b12378a787b22cd7118833aaa"},
    {34, "12a8d30dcf0037e60637163781783b24e1761880a465"},
    {24, "12a8d30dd200e7e8fd6bd24199783b24cdb17b904fbb"},
    {14, "12a8d30dd90043ec066b12378b783b24cd71189921cc"},
    {4, "12a8d30ddc00e7a8b36b12378a783b24cd7118995a5e"},
};
static const ref_t fs_hold[6] = {
    {25, "12a8d30dd38023e3066b12378b8c3b24cd7118822f32"},
    {15, "12a8d30dd680e7a8456b12378a787b4bcd7118839a93"},
    {5, "12a8d30ddd8037e606c7143781783b2451771880b740"},
    {32, "12a8d30dce00e7e8fd6bd25a99783b24cdb1529013c0"},
    {22, "12a8d30dd500e7e8066b12378f783b24cd7118993564"},
    {12, "12a8d30dd800e7e8fd6b12378e783b24cd711899f659"},
};

static void check_cycle(const ref_t* ref, const uint16_t* ch, uint16_t fs_base, uint16_t hold)
{
  uint16_t fs[FASST_CHANNELS];
  for (int i = 0; i < FASST_CHANNELS; i++) fs[i] = fs_base - 150 * i;
  for (int t = 0; t < FASST_FS_TYPES; t++) {
    uint8_t want[FASST_PACKET_LEN], p[FASST_PACKET_LEN];
    hex(ref[t].hex, want);
    fasst_packet(p, ref[t].k, FASST_HOP_STEP, ch, t & 1, t + 1, fs, hold);
    CHECK(!memcmp(p, want, sizeof(p)));
    if (t) CHECK(fasst_next_hop(ref[t - 1].k, FASST_HOP_STEP) == ref[t].k);
  }
}

// Despread: each 11 chips against the Barker code, bit by bit
static int despread(const uint8_t* chips, uint8_t* bits, int n)
{
  int worst = 11;
  for (int i = 0; i < n; i++) {
    int corr = 0;
    for (int c = 0; c < 11; c++) {
      int j = 11 * i + c;
      int chip = chips[j / 8] >> (7 - j % 8) & 1;
      corr += chip == (0x712 >> (10 - c) & 1) ? 1 : -1;
    }
    bits[i] = corr < 0;
    if (abs(corr) < worst) worst = abs(corr);
  }
  return worst;
}

int main()
{
  uint16_t ch[FASST_CHANNELS];
  for (int i = 0; i < FASST_CHANNELS; i++) ch[i] = 200 + 170 * i;

  check_cycle(fs_all, ch, 1900, 0);
  check_cycle(fs_hold, ch, 1500, 0x300);

  uint8_t p[FASST_PACKET_LEN], want[FASST_PACKET_LEN];
  fasst_packet(p, 8, FASST_HOP_STEP, ch, false, 0, NULL, 0);
  hex("12a8d30dda0037e6066b123790783b24cd711899984f", want);
  CHECK(!memcmp(p, want, sizeof(p)));
  fasst_packet(p, 35, FASST_HOP_STEP, ch, true, 0, NULL, 0);
  hex("12a8d30dcc80e7e8fd6b123790783b24cd7118991c84", want);
  CHECK(!memcmp(p, want, sizeof(p)));

  // Hops: 36 channels, each once per period, CHQ 2349..2419
  uint8_t seen[FASST_HOPS] = {0}, k = FASST_HOP_FIRST;
  for (int i = 0; i < FASST_HOPS; i++, k = fasst_next_hop(k, FASST_HOP_STEP)) {
    CHECK(k < FASST_HOPS);
    if (k < FASST_HOPS) seen[k]++;
  }
  CHECK(k == FASST_HOP_FIRST);
  for (int i = 0; i < FASST_HOPS; i++) CHECK(seen[i] == 1);
  CHECK(fasst_freq_khz(0) == 2349 * 1024 && fasst_freq_khz(35) == 2419 * 1024);

  // Chips: preamble 1010..10, sync 0x2BB1 bit 0 first, the packet LSB first
  uint8_t chips[FASST_CHIPS_LEN], bits[256];
  fasst_spread(chips, p);
  CHECK(despread(chips, bits, 256) == 11);
  int ok = 1;
  for (int i = 0; i < 64; i++) ok &= bits[i] == (~i & 1);
  for (int i = 0; i < 16; i++) ok &= bits[64 + i] == (0x2BB1 >> i & 1);
  for (int i = 0; i < 176; i++) ok &= bits[80 + i] == (p[i / 8] >> (i % 8) & 1);
  CHECK(ok);
  CHECK(fasst_sync_word(chips) == ((uint32_t)chips[0] << 24 | chips[1] << 16 | chips[2] << 8 | chips[3]));
  return test_done();
}
