// Futaba FASST, 10ch (fasst_frame.h): one packet per 7 ms hop, the radio's
// channels 1..10 as FASST ch1..10, no telemetry.
//
// - ID: 0x12 (as the TM-10's, both modes), then the low 24 bits of the
//   module id + rx_num: one receiver binding per model. A FASST transmitter
//   has no bind of its own: the receiver links to the transmitter nearby
//   while its Link button is held (R6008HS: > 2 s, receiver on). Bind ends
//   at once.
// - Channels: +-100% (MPM_CHANNEL_MIN..MAX) = 1020..2020 us as SFHSS
//   (values 224..1824, 1024 = 1520 us); +-125% fits in the 11 bits.
// - Failsafe: the F/S cycle (6 frames, one per channel pair) once per
//   second, from the radio's failsafe values (hold and no pulse: NOR, the
//   channel holds). One cycle sets the receiver's failsafe, which a power
//   cycle of the receiver forgets. None until the radio sends failsafe.
// - Power: full, low power 9 dB down, range check 34 dB down (as the
//   CC2500 emulation).
#include "protocols.h"

#ifdef FASST_INO
#include "board.h"
#include "fasst_frame.h"
#include "radio.h"
#include "timer.h"
#include "mpm_compat.h"  // rf_power_word()

// The packet's air time starts this long after the callback's deadline:
// building and spreading it take ~100 us, the deadline sets the hop timing
#define FASST_TX_DELAY_US 1000
// F/S cycle period, frames (~1 s)
#define FASST_FS_EVERY 143

static uint8_t _chips[FASST_CHIPS_LEN];
static uint8_t _hop;
static uint32_t _frame;
static uint16_t _power;

static uint16_t _value(uint16_t ch)
{
  int32_t v = 224 + ((int32_t)ch - MPM_CHANNEL_MIN) * 1600 / (MPM_CHANNEL_MAX - MPM_CHANNEL_MIN);
  return v < 0 ? 0 : v > FASST_VALUE_MAX ? FASST_VALUE_MAX : v;
}

static uint16_t _tx_power()
{
  return rf_power_word(mpm.range_check ? 34 : mpm.low_power ? 9 : 0);
}

void FASST_init()
{
  _power = _tx_power();
  radio_phy_t phy;
  fasst_phy(&phy, _power);
  fasst_set_id(FASST_ID_BYTE0 << 24 | (mpm.id & 0xFFFFFF));
  _hop = FASST_HOP_FIRST;
  _frame = 0;
  radio_set_frequency(fasst_freq_khz(_hop));
  if (radio_init(&phy) != RADIO_OK) mpm_sub_invalid();
  if (mpm.bind_in_progress) mpm_bind_done();
}

uint16_t FASST_callback()
{
  uint16_t power = _tx_power();
  if (power != _power) {
    _power = power;
    radio_set_tx_power(power);
  }
  radio_set_frequency_start(fasst_freq_khz(_hop));

  uint16_t ch[FASST_CHANNELS], fs[FASST_CHANNELS];
  uint16_t hold = 0;
  for (int i = 0; i < FASST_CHANNELS; i++) {
    ch[i] = _value(mpm.channels[i]);
    uint16_t f = mpm.failsafe[i];
    if (f == MPM_FAILSAFE_HOLD || f == MPM_FAILSAFE_NO_PULSE) hold |= 1 << i;
    fs[i] = _value(f);
  }
  uint8_t type = 0;
  uint32_t n = _frame % FASST_FS_EVERY;
  if (mpm.failsafe_valid && n < FASST_FS_TYPES) type = n + 1;

  uint8_t pkt[FASST_PACKET_LEN];
  fasst_packet(pkt, _hop, FASST_HOP_STEP, ch, _frame & 1, type, fs, hold);
  fasst_spread(_chips, pkt);
  radio_set_sync_word(fasst_sync_word(_chips), 0);
  radio_tx_start_at(fasst_chips(_chips), FASST_CHIPS_TX_LEN,
                    mpm_callback_deadline() + FASST_TX_DELAY_US);

  _hop = fasst_next_hop(_hop, FASST_HOP_STEP);
  _frame++;
  return FASST_FRAME_US;
}
#endif
