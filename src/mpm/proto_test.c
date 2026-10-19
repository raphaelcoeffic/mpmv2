// Test protocol: NRF24L01 ShockBurst packets with the channels, every 4 ms,
// for the bench to check the MULTI-Module core (timing, channels, flags).
// Sub protocols: 1 Mbps, 250 kbps. Option: RF channel (0..125)
//
// Packet (TX_ADDR 32 76 4d 50 4d: "MPMv2" on air, CRC16):
//   0     sequence number
//   1     flags: TEST_BIND, TEST_RANGE, TEST_LOW_POWER, TEST_FAILSAFE
//   2     rx_num
//   3     sub protocol
//   4-25  16 channels, 11 bits, as in the frames of the radio
//   26-29 micros() at the callback, little endian
#include "protocols.h"

#include <string.h>

#include "board.h"
#include "frame.h"
#include "radio.h"
#include "timer.h"

#define TEST_PERIOD_US 4000
#define TEST_PAYLOAD_LEN 30
#define TEST_CHANNEL_DEFAULT 40

#define TEST_BIND 0x80
#define TEST_RANGE 0x40
#define TEST_LOW_POWER 0x20
#define TEST_FAILSAFE 0x10

#define NRF_FREQ_BASE_KHZ 2400000
#define NRF_CH_MAX 125

static frame_t _frame;
static uint8_t _seq;
static uint16_t _bind_count;
static int8_t _option;
static uint8_t _air[FRAME_AIR_MAX];

static uint8_t _rf_channel()
{
  return _option >= 0 && _option <= NRF_CH_MAX ? _option : TEST_CHANNEL_DEFAULT;
}

void TEST_init()
{
  memset(&_frame, 0, sizeof(_frame));
  _frame.type = FRAME_NRF;
  memcpy(_frame.addr, "2vMPM", 5);
  _frame.addr_len = 5;
  _frame.crc_len = 2;
  _frame.sync_bytes = 4;
  if (!frame_init(&_frame)) {
    mpm_sub_invalid();
    return;
  }

  bool slow = mpm.sub_protocol == 1;
  radio_phy_t phy = {
    .symbol_rate = slow ? 250000 : 1000000,
    .deviation = 160000,
    .rx_bw = slow ? 94 : 97,
    .preamble_bytes = 4,
    .sync_bits = frame_sync_bits(&_frame),
    .if_freq = (int16_t)0x8000,
    .tx_power = RF_TX_POWER_DEFAULT,
  };
  _option = mpm.option;
  radio_set_frequency(NRF_FREQ_BASE_KHZ + _rf_channel() * 1000);
  radio_set_sync_word(frame_tx_sync(&_frame), frame_rx_sync(&_frame));
  if (radio_init(&phy) != RADIO_OK) mpm_sub_invalid();
  _seq = 0;
  _bind_count = 0;
}

uint16_t TEST_callback()
{
  uint32_t now = micros();
  if (mpm.option != _option) {
    _option = mpm.option;
    radio_set_frequency(NRF_FREQ_BASE_KHZ + _rf_channel() * 1000);
  }

  uint8_t p[TEST_PAYLOAD_LEN] = {0};
  p[0] = _seq++;
  p[1] = (mpm.bind_in_progress ? TEST_BIND : 0) | (mpm.range_check ? TEST_RANGE : 0) |
         (mpm.low_power ? TEST_LOW_POWER : 0) | (mpm.failsafe_valid ? TEST_FAILSAFE : 0);
  p[2] = mpm.rx_num;
  p[3] = mpm.sub_protocol;
  for (uint8_t i = 0; i < MPM_CHANNELS; i++) {
    uint16_t bit = 11 * i;
    uint8_t* c = p + 4 + bit / 8;
    uint32_t v = (uint32_t)mpm.channels[i] << (bit % 8);
    c[0] |= v;
    c[1] |= v >> 8;
    c[2] |= v >> 16;
  }
  memcpy(p + 26, &now, 4);

  // Bind: 2 s, as the protocols that end it on their own
  if (mpm.bind_in_progress && ++_bind_count > 500) {
    _bind_count = 0;
    mpm_bind_done();
  }

  uint16_t len = frame_tx_data(&_frame, p, sizeof(p), _air);
  if (len) radio_tx_start(_air, len);
  return TEST_PERIOD_US;
}
