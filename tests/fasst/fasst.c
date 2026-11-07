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

  uint8_t plain[FASST_PACKET_LEN], uk, ustep;

  // MULT vs the T14SG (doc/futaba/t14sg/mult_idle_20s.txt, all on hold):
  // ch1..8, ch9..16, then the F/S cycle, consecutive frames, step 5
  static const uint16_t mch[FASST_MULT_CHANNELS] = {1029, 1017, 1696, 1024, 352, 1029, 1696, 1024,
                                                     1696, 1024, 1024, 1024, 112, 112, 1024, 1024};
  static const ref_t mult[10] = {
      {0, "67c2378dde1efa522ed7765b9c68c925a9930e952d8a"},
      {5, "67c2378ddd9e5fc8d1777cdb9c7890e209998e956d07"},
      {10, "67c2378ddb1e5f582ed7765b8f78c025a9930e8657c7"},
      {15, "67c2378dd69e5fc8d1777cdb8e7890e209998e870cae"},
      {20, "67c2378dd41efa522e777c5b8d68c92509990e84a685"},
      {25, "67c2378dd39e5fc8d1777cdb9d7890e209998e94ccfb"},
      {30, "67c2378dd11efa522ed7765b8f68c925a9930e86189b"},
      {35, "67c2378dcc9e5f582e777cdb8e78c02509998e8775a3"},
      {3, "67c2378ddc9efa522ed7765b8d68c925a9930e84a67a"},
      {8, "67c2378dda1e5fc8d1777c5b9d7890e209990e947652"},
  };
  fasst_set_id(0x67027B60);
  for (int t = 0; t < 10; t++) {
    fasst_mult_packet(p, mult[t].k, 5, mch, t & 1, t < 2 ? 0 : t - 1, NULL, 0xFFFF);
    hex(mult[t].hex, want);
    CHECK(!memcmp(p, want, sizeof(p)));
  }
  // F/S on ch3 and ch7 (type 7): their slots take the F/S values, plain
  // bit 2 set
  uint16_t mfs[FASST_MULT_CHANNELS];
  for (int i = 0; i < FASST_MULT_CHANNELS; i++) mfs[i] = 300 + i;
  fasst_mult_packet(p, 4, 5, mch, false, 7, mfs, 0xFFFF & ~(1 << 2 | 1 << 6));
  CHECK(fasst_unpack(p, plain, &uk, &ustep) && uk == 4 && plain[12] == 0x86 && plain[19] == 0xA6);
  CHECK((plain[9] | (plain[10] & 7) << 8) == 302 && (plain[16] | (plain[17] & 7) << 8) == 306);

  // 7CH vs the T14SG (doc/futaba/t14sg/7ch_*.txt): idle, its F/S packet
  // (hold), throttle F/S at +66 % (F/S packet, normal), a sweep frame, hop
  // 35, France (step 61)
  static const struct {
    uint8_t k, step;
    bool fs_packet;
    int16_t fs3;
    uint16_t ch[FASST7_CHANNELS];
    const char* hex;
  } ref7[] = {
      {0, 5, false, -1, {1323, 1315, 1740, 1320, 900, 1323, 1740}, "6782f329c2b0895d8afb62bf489738d608"},
      {0, 5, true, -1, {1323, 1315, 1740, 1320, 900, 1323, 1740}, "6782f329c2b0895d8afb62bf48973b4d3a"},
      {0, 29, true, 1600, {1324, 1316, 1740, 1320, 900, 1324, 1740}, "6782f329c2888e4c0afb62bf3897fae6cf"},
      {0, 29, false, 1600, {1324, 1316, 1740, 1320, 900, 1324, 1740}, "6782f329c2888e5d8afb62bf3897f9a7a1"},
      {0, 5, true, -1, {1322, 1316, 1543, 1320, 900, 1322, 1543}, "6782f329c2b88e44eafb62bf59013bbc7b"},
      {35, 5, false, -1, {1323, 1315, 1740, 1320, 900, 1323, 1740}, "6782f33b42b0895d8afb62bf489738ae8d"},
      {1, 61, false, -1, {1324, 1315, 1740, 1320, 900, 1324, 1740}, "6782f3284288895d8afb62bf3896f85a57"},
  };
  uint8_t p7[FASST7_PACKET_LEN], want7[FASST7_PACKET_LEN], plain7[FASST7_PACKET_LEN], code;
  uint16_t ch7[FASST7_CHANNELS];
  for (unsigned t = 0; t < sizeof(ref7) / sizeof(ref7[0]); t++) {
    fasst7_packet(p7, ref7[t].k, ref7[t].step, ref7[t].ch, ref7[t].fs_packet, ref7[t].fs3);
    hex(ref7[t].hex, want7);
    CHECK(!memcmp(p7, want7, sizeof(p7)));
    CHECK(fasst7_unpack(want7, plain7, &uk, &ustep, ch7, &code) && uk == ref7[t].k &&
          ustep == ref7[t].step);
    CHECK(code == (ref7[t].fs_packet ? 2 : 0) + ((ref7[t].fs3 < 0) == ref7[t].fs_packet));
    CHECK(ch7[2] == (ref7[t].fs_packet && ref7[t].fs3 >= 0 ? ref7[t].fs3 : ref7[t].ch[2]));
  }
  want7[6] ^= 1;
  CHECK(!fasst7_unpack(want7, plain7, &uk, &ustep, NULL, NULL));
  fasst_set_id(FASST_ID_DEFAULT);

  // Unpack: the hop index and step back; a flipped bit fails the CRC
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

  // FASSTest 14CH TX packet: the T14SG's ID, row 21 hop 0 (channel 68), ch
  // 352 + 84 i (checked with doc/futaba/t14sg/dec14.py: CRC ok, ch1..8)
  {
    const uint8_t id[FASSTEST_ID_LEN] = {0x08, 0x8A, 0xAF, 0xFB, 0x00, 0x00};
    uint16_t v[FASSTEST_TX_CHANNELS];
    for (int i = 0; i < FASSTEST_TX_CHANNELS; i++) v[i] = 352 + 84 * i;
    uint8_t pk[FASSTEST_PACKET_LEN], want[FASSTEST_PACKET_LEN], set, row;
    CHECK(fasstest_channel(0, 21, 0) == 68 && fasstest_channel(0, 21, 1) == 11);
    fasstest_packet(pk, false, id, fasstest_code(0, 21), 68, v, false, 0);
    hex("40cb0c9c1b7a205c46238e2cceac0344088af7c1", want);
    CHECK(!memcmp(pk, want, sizeof(pk)));
    CHECK(fasstest_hop_code(pk, &set, &row) && set == 0 && row == 21);
    // The T14SG's link packet (ft14_link.txt)
    fasstest_link_packet(pk, false, id, fasstest_code(0, 21), FASSTEST_LINK_CHANNEL(0),
                         FASSTEST_BFS_DEFAULT);
    hex("483b088aaffb0000260200000000000200002185", want);
    CHECK(!memcmp(pk, want, sizeof(pk)));
    // A page 0 request: plain b1 bit 5, b16 = 1
    uint8_t plain[18];
    fasstest_packet(pk, false, id, fasstest_code(0, 21), 68, v, false, 1);
    CHECK(fasstest_reply_unpack(pk, false, plain, id, id, 68) && plain[1] == 0xEB &&
          plain[16] == 0x01);
    // An R7008SB reply (serial 08 a1 82 40) on channel 47, page 0: slot 0 =
    // 5.1 V, quality 3; slots 1..4 = the sbus2mon sensors (ft14_sens.txt)
    const uint8_t rx[4] = {0x08, 0xA1, 0x82, 0x40};
    hex("3006c892ffc0e803af42a1fa4000002f08a1292b", pk);
    CHECK(fasstest_reply_unpack(pk, false, plain, rx, id, 47));
    hex("3006c0337d80e803804a0078000000000000", want);
    CHECK(!memcmp(plain, want, 18));
    CHECK(!fasstest_reply_unpack(pk, false, plain, rx, id, 46));
    // The same reply rebuilt (the receiver emulator's): b0..1 = the TX's hop
    // code (row 11), type 0x10, the page
    uint8_t re[FASSTEST_PACKET_LEN];
    fasstest_reply_packet(re, false, 49, 0, want + 2, rx, id, 47);
    CHECK(!memcmp(re, pk, sizeof(re)));
    // Its serial without a link: bytes 9..12 plain 0 (slots 3..5 hold
    // sensors: not this one); a page 1 reply (slot 8 only, ft14_sens2.txt)
    uint8_t got[4];
    CHECK(!fasstest_reply_serial(pk, false, id, 47, got));
    hex("3016489d824000001408a1824000001408a1b77b", pk);
    CHECK(fasstest_reply_serial(pk, false, id, 20, got) && !memcmp(got, rx, 4));
    CHECK(fasstest_reply_unpack(pk, false, plain, rx, id, 20));
    fasstest_reply_packet(re, false, 49, 1, plain + 2, rx, id, 20);
    CHECK(!memcmp(re, pk, sizeof(re)));
    // The R7008SB's link replies (ft14_link3.txt, 12CH ft14_link2.txt)
    fasstest_link_packet(pk, false, id, fasstest_code(0, 21), 2, 40);
    CHECK(fasstest_link_check(pk, 2) && !fasstest_link_check(pk, 8));
    fasstest_link_reply(re, pk, rx);
    hex("f800088aaffb0000280208a18240000200003ea9", want);
    CHECK(!memcmp(re, want, sizeof(re)));
    fasstest_link_packet(pk, true, id, 49, 2, 45);
    fasstest_link_reply(re, pk, rx);
    hex("f800088aaffb00002d0208a1824002020000f30c", want);
    CHECK(!memcmp(re, want, sizeof(re)));
    pk[3] ^= 1;
    CHECK(!fasstest_link_check(pk, 2));

    // F/S packet: ch3 F/S at 1365 (slot 2, mode field 0x02 | 1 << 2), ch5
    // B.F/S (slot 4: 0x20 | 2 << 2), as the T14SG's (ft14_fs.txt)
    fasstest_fs_packet(pk, false, id, fasstest_code(0, 21), 68, v, 2, FASSTEST_FS_FS, 1365);
    CHECK(fasstest_reply_unpack(pk, false, plain, id, id, 68));
    CHECK((plain[1] & 0xC0) == 0x80 && (plain[2] & 0x0F) == 0x01);
    CHECK(((plain[5] >> 4) | (plain[6] & 0x7F) << 4) == 1365 && !(plain[6] & 0x80));
    fasstest_fs_packet(pk, false, id, fasstest_code(0, 21), 68, v, 4, FASSTEST_FS_BFS, 1024);
    CHECK(fasstest_reply_unpack(pk, false, plain, id, id, 68));
    CHECK((plain[1] & 0xC0) == 0x00 && (plain[2] & 0x0F) == 0x0A);

    // 12CH (ft12_sens.txt): row 11 (code 49), ch1..5 on channel 38, ch6..10
    // on 50; the link packet's b14 bit 1
    const uint16_t v12[FASSTEST_TX_CHANNELS_12CH] = {1032, 1018, 1678, 1024, 352,
                                                     1032, 1678, 1024, 1696, 1024};
    fasstest_packet(pk, true, id, 49, 38, v12, false, 0);
    hex("23c68cca55186800221e21f5", want);
    CHECK(!memcmp(pk, want, FASSTEST_PACKET_LEN_12CH));
    fasstest_packet(pk, true, id, 49, 50, v12, true, 0);
    hex("23c68c4a21f5c0a03cc8faf7", want);
    CHECK(!memcmp(pk, want, FASSTEST_PACKET_LEN_12CH));
    fasstest_link_packet(pk, true, id, 49, 2, 45);
    CHECK(pk[14] == 0x02 && pk[8] == 45);
    // A 12CH reply (channel 65: 5.0 V, 99 %): its serial from bytes 9 and
    // 5 and the CRC, confirmed on another reply (channel 2: EXT 0 V, 100 %)
    hex("3306c893e14000004108bf6a", pk);
    CHECK(fasstest_reply_serial(pk, true, id, 65, got) && !memcmp(got, rx, 4));
    CHECK(fasstest_reply_unpack(pk, true, plain, got, id, 65) && plain[3] == 0x32 &&
          plain[4] == 99);
    hex("3306cca1e640000002084181", pk);
    CHECK(fasstest_reply_unpack(pk, true, plain, got, id, 2) && plain[4] == 100);
    fasstest_reply_packet(re, true, 49, 0, plain + 2, got, id, 2);
    CHECK(!memcmp(re, pk, FASSTEST_PACKET_LEN_12CH));
  }
  return test_done();
}
