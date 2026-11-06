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

  // Unpack: the hop index and step back; a flipped bit fails the CRC
  uint8_t plain[FASST_PACKET_LEN], uk, ustep;
  for (uint8_t k = 0; k < FASST_HOPS; k++) {
    fasst_packet(p, k, 28, ch, k & 1, 0, NULL, 0);
    CHECK(fasst_unpack(p, plain, &uk, &ustep) && uk == k && ustep == 28);
  }
  CHECK(plain[0] == FASST_ID_BYTE0);
  p[7] ^= 0x10;
  CHECK(!fasst_unpack(p, plain, &uk, &ustep));

  // Hops: 36 channels, each once per period, CHQ 2349..2419
  uint8_t seen[FASST_HOPS] = {0}, k = FASST_HOP_FIRST;
  for (int i = 0; i < FASST_HOPS; i++, k = fasst_next_hop(k, FASST_HOP_STEP)) {
    CHECK(k < FASST_HOPS);
    if (k < FASST_HOPS) seen[k]++;
  }
  CHECK(k == FASST_HOP_FIRST);
  for (int i = 0; i < FASST_HOPS; i++) CHECK(seen[i] == 1);
  CHECK(fasst_freq_khz(0) == 2349 * 1024 && fasst_freq_khz(35) == 2419 * 1024);

  // France (T14SG): step field 61 = k - 2, 62 = k - 1, 41 = k + 1, hops 1..22
  CHECK(fasst_next_hop(1, 61) == 22 && fasst_next_hop(2, 61) == 21 && fasst_next_hop(3, 61) == 1);
  CHECK(fasst_next_hop(1, 62) == 22 && fasst_next_hop(22, 41) == 1);
  for (uint8_t s = 1; s <= FASST_FRANCE_HOPS; s++) {
    uint8_t fseen[FASST_FRANCE_HOPS + 1] = {0};
    k = 1;
    for (int i = 0; i < FASST_FRANCE_HOPS; i++, k = fasst_next_hop(k, FASST_FRANCE_STEP + s)) {
      CHECK(k >= 1 && k <= FASST_FRANCE_HOPS);
      if (k <= FASST_FRANCE_HOPS) fseen[k]++;
    }
    CHECK(k == 1);
    for (int i = 1; i <= FASST_FRANCE_HOPS; i++) CHECK(fseen[i] == 1);
  }
  fasst_packet(p, 1, 61, ch, false, 0, NULL, 0);
  CHECK(fasst_unpack(p, plain, &uk, &ustep) && uk == 1 && ustep == 61);
  fasst_packet(p, 0, 61, ch, false, 0, NULL, 0);
  CHECK(!fasst_unpack(p, plain, &uk, &ustep));
  fasst_packet(p, 5, 63, ch, false, 0, NULL, 0);
  CHECK(!fasst_unpack(p, plain, &uk, &ustep));

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

  // RX: the sync word = the 32 chips from sync bit 3, then the rest of the
  // sync and the packet, despread with one chip flipped per bit
  uint32_t c0 = (64 + 3) * 11, sw = 0;
  for (int i = 0; i < 32; i++) sw = sw << 1 | (chips[(c0 + i) / 8] >> (7 - (c0 + i) % 8) & 1);
  CHECK(fasst_rx_sync_word(FASST_BARKER_11) == sw);
  CHECK(FASST_RX_SKIP_CHIPS(11) == 1 && FASST_RX_SYNC_LEFT(11) == 10);
  uint8_t rx[FASST_CHIPS_LEN + 2] = {0}, out[32], corr[FASST_RX_SYNC_LEFT(11) + 176];
  uint32_t n_rx = FASST_CHIPS_LEN * 8 - c0 - 32;
  for (uint32_t i = 0; i < n_rx; i++) {
    uint32_t c = c0 + 32 + i;
    uint8_t v = (chips[c / 8] >> (7 - c % 8) & 1) ^ ((i + 11 - FASST_RX_SKIP_CHIPS(11)) % 11 == 5);
    if (v) rx[i / 8] |= 0x80 >> (i % 8);
  }
  fasst_despread(rx, FASST_BARKER_11, FASST_RX_SKIP_CHIPS(11), FASST_RX_SYNC_LEFT(11) + 176, out, corr);
  ok = 1;
  for (int i = 0; i < FASST_RX_SYNC_LEFT(11) + 176; i++) {
    int b = i < FASST_RX_SYNC_LEFT(11) ? fasst_rx_sync_left(11) >> i & 1
                                       : p[(i - FASST_RX_SYNC_LEFT(11)) / 8] >> ((i - FASST_RX_SYNC_LEFT(11)) % 8) & 1;
    ok &= (out[i / 8] >> (i % 8) & 1) == b && corr[i] == 9;
  }
  CHECK(ok);

  // 7 chips (FASSTest 12CH): sync 0x2BB1 and a 12-byte packet spread with
  // Barker-7 1110010 (0 = the code, 1 = inverted); the RX sync word = the
  // 32 chips from sync bit 3, then the rest, one chip flipped per bit
  static const uint8_t p12[12] = {0x40, 0xCB, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x55, 0xAA};
  uint8_t c7[(16 + 96) * 7 / 8 + 3] = {0};
  uint32_t nc = 0;
  for (int i = 0; i < 16 + 96; i++) {
    int d = i < 16 ? 0x2BB1 >> i & 1 : p12[(i - 16) / 8] >> ((i - 16) % 8) & 1;
    uint32_t code = d ? ~0x72u : 0x72u;
    for (int c = 6; c >= 0; c--, nc++)
      if (code >> c & 1) c7[nc / 8] |= 0x80 >> (nc % 8);
  }
  sw = 0;
  for (int i = 0; i < 32; i++) sw = sw << 1 | (c7[(21 + i) / 8] >> (7 - (21 + i) % 8) & 1);
  CHECK(fasst_rx_sync_word(FASST_BARKER_7) == sw);
  CHECK(FASST_RX_SKIP_CHIPS(7) == 3 && FASST_RX_SYNC_LEFT(7) == 8);
  uint8_t rx7[sizeof(c7)] = {0}, out7[16], corr7[8 + 96];
  for (uint32_t i = 0; i < nc - 21 - 32; i++) {
    uint32_t c = 21 + 32 + i;
    uint8_t v = (c7[c / 8] >> (7 - c % 8) & 1) ^ ((i + 7 - FASST_RX_SKIP_CHIPS(7)) % 7 == 2);
    if (v) rx7[i / 8] |= 0x80 >> (i % 8);
  }
  fasst_despread(rx7, FASST_BARKER_7, FASST_RX_SKIP_CHIPS(7), 8 + 96, out7, corr7);
  ok = 1;
  for (int i = 0; i < 8 + 96; i++) {
    int b = i < 8 ? fasst_rx_sync_left(7) >> i & 1 : p12[(i - 8) / 8] >> ((i - 8) % 8) & 1;
    ok &= (out7[i / 8] >> (i % 8) & 1) == b && corr7[i] == 5;
  }
  CHECK(ok);

  // FASSTest: the T14SG's hop code (bytes 40 cb, its reply 50 0b) = set A
  // row 21; the slot order heard on the even channels (2026-11-06 scan)
  uint8_t set, row, next, tx[2] = {0x40, 0xCB}, reply[2] = {0x50, 0x0B}, bad[2] = {0x00, 0xC0};
  CHECK(fasstest_hop_code(tx, &set, &row) && set == 0 && row == 21);
  CHECK(fasstest_hop_code(reply, &set, &row) && set == 0 && row == 21);
  CHECK(!fasstest_hop_code(bad, &set, &row));
  static const uint8_t order[FASSTEST_HOPS] = {68, 11, 2,  26, 8,  56, 20, 47, 44, 29, 23,
                                               62, 50, 59, 35, 53, 5,  41, 14, 17, 32, 38};
  ok = 1;
  for (int i = 0; i < FASSTEST_HOPS; i++)
    ok &= fasstest_next_channel(0, 21, order[i], &next) && next == order[(i + 1) % FASSTEST_HOPS];
  CHECK(ok);
  CHECK(!fasstest_next_channel(0, 21, 65, &next));  // the row's unused channel
  CHECK(fasstest_freq_khz(2) == fasst_freq_khz(0));
  return test_done();
}
