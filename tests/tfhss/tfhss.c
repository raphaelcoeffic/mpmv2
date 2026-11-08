// T-FHSS hops and packets: rows 0, 21 and 30 and the 16-channel list as the
// R334SBS's tables, a packet as doc/futaba's loopback reference (sent
// CC2652 <-> CC2500)
#include <string.h>
#include "test.h"
#include "tfhss.h"

static void check_row(uint8_t row, const char* want_hex)
{
  uint8_t want[TFHSS_CHANNELS];
  hex(want_hex, want);
  for (int i = 0; i < TFHSS_CHANNELS; i++) {
    CHECK(tfhss_channel(row, i) == want[i]);
    CHECK(tfhss_next(row, want[i]) == want[(i + 1) % TFHSS_CHANNELS]);
  }
}

int main(void)
{
  check_row(0, "1f0103091b131a1011141d190d08180a1e1c16040c050f0e0b020612170715");
  check_row(21, "1516181e11091006070a130f031d0e1f14120c19021a050401171b080d1c0b");
  check_row(30, "1e1f02081a12190f10131c180c0717091d1b15030b040e0d0a010511160614");
  uint8_t list[16];
  hex("0119031604150613071209100a0f0c0d", list);
  for (int i = 0; i < 16; i++) CHECK(tfhss_next(TFHSS_ROW_LIST, list[i]) == list[(i + 1) % 16]);
  // Not in the list: its first, as the receiver
  CHECK(tfhss_next(TFHSS_ROW_LIST, 31) == 1);
  // Every row a permutation of 1..31
  for (int r = 0; r < TFHSS_ROWS; r++) {
    uint32_t seen = 0;
    for (int i = 0; i < TFHSS_CHANNELS; i++) seen |= 1u << tfhss_channel(r, i);
    CHECK(seen == 0xFFFFFFFEu);
  }

  // Type 0, row 8 (byte 1 = 0x21), ID 12345678, x 5A, b5..b8 01 02 03 04,
  // on channels 10 and 22
  uint8_t plain[TFHSS_DATA_LEN] = {0x00, 0x21, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  uint8_t key[TFHSS_DATA_LEN] = {0, 0, 0x12, 0x34, 0x56, 0x78, 0x5A, 10, 1, 2, 3, 4};
  uint8_t p[TFHSS_PACKET_LEN], want[TFHSS_PACKET_LEN];
  tfhss_packet(p, plain, key);
  hex("00211235547b5e0f07050b0da064", want);
  CHECK(!memcmp(p, want, sizeof(p)));
  CHECK(tfhss_type(p) == 0 && tfhss_row(p) == 8);
  CHECK(tfhss_syndrome(p) == 0xB144);
  uint16_t ks = tfhss_key_syndrome(p, 10);
  key[7] = 22;
  tfhss_packet(p, plain, key);
  hex("00211235547b5e1307050b0da064", want);
  CHECK(!memcmp(p, want, sizeof(p)));
  CHECK(tfhss_syndrome(p) == 0x7234);
  CHECK(tfhss_key_syndrome(p, 22) == ks && tfhss_key_syndrome(p, 21) != ks);
  // The key's CRC from 0, without the channel
  CHECK(ks == 0xFDEC);
  // Plain (bind): 0
  memcpy(p, plain, TFHSS_DATA_LEN);
  uint16_t crc = tfhss_crc(plain, TFHSS_DATA_LEN, 0xFFFF);
  p[12] = crc;
  p[13] = crc >> 8;
  CHECK(tfhss_syndrome(p) == 0);
  return test_done();
}
