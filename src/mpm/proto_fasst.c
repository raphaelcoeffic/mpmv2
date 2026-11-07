// Futaba FASST and FASSTest (fasst_frame.h). Sub protocols:
//
// 10ch: FASST 10ch, one packet per 7 ms hop, the radio's channels 1..10 as
// FASST ch1..10, no telemetry.
// - ID: 0x12 (as the TM-10's, both modes), then the low 24 bits of the
//   module id + rx_num: one receiver binding per model. A FASST transmitter
//   has no bind of its own: the receiver links to the transmitter nearby
//   while its Link button is held (R6008HS: > 2 s, receiver on). Bind ends
//   at once.
// - Step: picked at each start, a random one (1..36) no FASST transmitter
//   heard on hop FASST_LISTEN_HOP in FASST_LISTEN_US uses (as the T14SG
//   at power-up); the receiver follows the step the packets carry.
// - Failsafe: the F/S cycle (6 frames, one per channel pair) once per
//   second, from the radio's failsafe values (hold and no pulse: NOR, the
//   channel holds). One cycle sets the receiver's failsafe, which a power
//   cycle of the receiver forgets. None until the radio sends failsafe.
//
// MULT: FASST MULT (16 channels, the 10ch packet with every group's
// flags, as the T14SG), otherwise as 10ch; the F/S cycle has 8 frames
// (fasst_mult_packet()).
//
// 7ch: FASST 7CH (fasst7_packet()), one 17-byte packet per 8 ms hop on
// the 10ch hops and steps, the radio's channels 1..7, as the T14SG. Only
// ch3 has a failsafe (the radio's CH3 value; hold and no pulse: hold),
// sent in every FASST7_FS_EVERY-th packet, the others say whether it is set.
//
// FT18 / FT12: FASSTest 18CH (the T14SG's "14CH": 16 channels + DG1 /
// DG2, 15 ms hops) / 12CH (10 channels, 6.3 ms hops, Barker-7 chips), as
// the T14SG.
// - ID: ID0..3 = 100000000 + module id + rx_num mod 900000000 (the
//   receiver saves ID0..3 as 9 decimal digits), ID4..5 = 0; the hop row
//   (set A) from it.
// - Bind: link packets on the T14SG's schedule (fasstest_link_packet()),
//   each followed by a listen for the receiver's reply (powered up within
//   the bind: it saves the ID at once); the reply, with the receiver's
//   serial, ends the bind, the data start 2 hops later as the T14SG.
// - Failsafe: one F/S packet per channel in turn every FASSTEST_FS_EVERY
//   hops (values: F/S; hold and no pulse: HOLD), once the radio sent its
//   failsafe values.
// - Telemetry (unless disabled): a request every FASSTEST_REQ_EVERY hops
//   (12CH: _12CH), the next hop left out and listened to. 18CH asks for the
//   pages holding something (0: the receiver's voltage, always; another
//   page once a reply of it had a slot not 0), every FASSTEST_PROBE_EVERY-th
//   request one of the others in turn (a new sensor). Each reply goes to
//   the radio as it came (MPM_TELEM_FUTABA). The replies are whitened with
//   the receiver's serial: from the bind's reply, or else found in the
//   replies (fasstest_reply_serial()), forgotten after
//   FASSTEST_SERIAL_MISSES replies in a row with a bad CRC.
//
// Channels: +-100% (MPM_CHANNEL_MIN..MAX) = 1020..2020 us as SFHSS (values
// 224..1824, 1024 = 1520 us, the same in FASST and FASSTest); +-125% fits
// in the 11 bits. Power: full, low power 9 dB down, range check 34 dB down
// (as the CC2500 emulation).
#include "protocols.h"

#ifdef FASST_INO
#include <string.h>

#include "board.h"
#include "fasst_frame.h"
#include "mpm_compat.h"  // rf_power_word()
#undef sub_protocol      // its MULTI-Module name: mpm.sub_protocol here
#include "radio.h"
#include "timer.h"

// The packet's air time starts this long after the callback's deadline:
// building and spreading it take ~100 us, the deadline sets the hop timing
#define FASST_TX_DELAY_US 1000
// F/S cycle period, frames (~1 s)
#define FASST_FS_EVERY 143
// The step pick: listening there first (every transmitter passes each hop
// once per 252 ms); hop 35 = 2477 MHz, above Wi-Fi channel 11
#define FASST_LISTEN_HOP 35
#define FASST_LISTEN_US 500000
#define FASST_LISTEN_POLL_US 1000

#define FASSTEST_FS_EVERY 8
#define FASSTEST_REQ_EVERY 10       // 150 ms: the T14SG at its 0.3 s rate
#define FASSTEST_REQ_EVERY_12CH 48  // 302 ms: the same
#define FASSTEST_PROBE_EVERY 4
#define FASSTEST_SERIAL_MISSES 8
// After the reply's RX would have ended: margin before it is read
#define FASSTEST_RX_MARGIN_US 1500

enum { SUB_10CH, SUB_MULT, SUB_7CH, SUB_FT18, SUB_FT12 };

static uint8_t _chips[FASST_CHIPS_LEN];
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

static void _update_power()
{
  uint16_t power = _tx_power();
  if (power != _power) {
    _power = power;
    radio_set_tx_power(power);
  }
}

// FASST 7CH: us - 200, +-100 % = 1020..2020 us as 10ch
static uint16_t _value7(uint16_t ch)
{
  int32_t v = 820 + ((int32_t)ch - MPM_CHANNEL_MIN) * 1000 / (MPM_CHANNEL_MAX - MPM_CHANNEL_MIN);
  return v < 0 ? 0 : v > FASST_VALUE_MAX ? FASST_VALUE_MAX : v;
}

static radio_packet_t _pkt;

// FASST 10ch / MULT / 7ch
static uint8_t _hop, _step;
static uint32_t _frame;
static bool _listening;
static uint32_t _listen_start;
static uint64_t _heard;  // bit s: step s (1..36) heard

// The start: parked on FASST_LISTEN_HOP for FASST_LISTEN_US, the steps of
// the FASST transmitters there noted; then a random step not heard
static uint16_t _fasst_listen()
{
  while (radio_receive(&_pkt)) {
    uint8_t b[FASST_PACKET_LEN], plain[FASST_PACKET_LEN], k, step;
    if (fasst_rx_bytes(_pkt.data, FASST_BARKER_11, FASST_PACKET_LEN, b, NULL) >= 0 &&
        (fasst_unpack(b, plain, &k, &step) || fasst7_unpack(b, plain, &k, &step, NULL, NULL)) &&
        step < 37)
      _heard |= 1ull << step;
  }
  if (micros() - _listen_start < FASST_LISTEN_US) {
    if (!radio_rx_active()) radio_rx_start(FASST_RX_PKT_LEN(FASST_BARKER_11, FASST_PACKET_LEN));
    return FASST_LISTEN_POLL_US;
  }
  radio_rx_stop();
  uint8_t free = 0;
  for (int s = 1; s < 37; s++) free += !(_heard >> s & 1);
  // All taken: any
  uint64_t taken = free ? _heard : 0;
  long r = mpm_random(free ? free : 36);
  for (_step = 1; taken >> _step & 1 || r--; _step++) {}
  _listening = false;
  return FASST_LISTEN_POLL_US;
}

static uint16_t _fasst7_callback()
{
  uint16_t ch[FASST7_CHANNELS];
  for (int i = 0; i < FASST7_CHANNELS; i++) ch[i] = _value7(mpm.channels[i]);
  uint16_t f = mpm.failsafe[2];
  int16_t fs3 = !mpm.failsafe_valid || f == MPM_FAILSAFE_HOLD || f == MPM_FAILSAFE_NO_PULSE
                    ? -1
                    : _value7(f);

  uint8_t pkt[FASST7_PACKET_LEN];
  fasst7_packet(pkt, _hop, _step, ch, _frame % FASST7_FS_EVERY == 0, fs3);
  fasst_spread_len(_chips, pkt, FASST7_PACKET_LEN);
  radio_set_sync_word(fasst_sync_word(_chips), 0);
  radio_tx_start_at(fasst_chips(_chips), FASST7_CHIPS_LEN - 4,
                    mpm_callback_deadline() + FASST_TX_DELAY_US);

  _hop = fasst_next_hop(_hop, _step);
  _frame++;
  return FASST7_FRAME_US;
}

static uint16_t _fasst_callback()
{
  if (_listening) return _fasst_listen();
  _update_power();
  radio_set_frequency_start(fasst_freq_khz(_hop));
  if (mpm.sub_protocol == SUB_7CH) return _fasst7_callback();

  bool mult = mpm.sub_protocol == SUB_MULT;
  uint8_t n_ch = mult ? FASST_MULT_CHANNELS : FASST_CHANNELS;
  uint16_t ch[FASST_MULT_CHANNELS], fs[FASST_MULT_CHANNELS];
  uint16_t hold = 0;
  for (int i = 0; i < n_ch; i++) {
    ch[i] = _value(mpm.channels[i]);
    uint16_t f = mpm.failsafe[i];
    if (f == MPM_FAILSAFE_HOLD || f == MPM_FAILSAFE_NO_PULSE) hold |= 1 << i;
    fs[i] = _value(f);
  }
  uint8_t type = 0;
  uint32_t n = _frame % FASST_FS_EVERY;
  if (mpm.failsafe_valid && n < (mult ? FASST_MULT_FS_TYPES : FASST_FS_TYPES)) type = n + 1;

  uint8_t pkt[FASST_PACKET_LEN];
  if (mult) fasst_mult_packet(pkt, _hop, _step, ch, _frame & 1, type, fs, hold);
  else fasst_packet(pkt, _hop, _step, ch, _frame & 1, type, fs, hold);
  fasst_spread(_chips, pkt);
  radio_set_sync_word(fasst_sync_word(_chips), 0);
  radio_tx_start_at(fasst_chips(_chips), FASST_CHIPS_TX_LEN,
                    mpm_callback_deadline() + FASST_TX_DELAY_US);

  _hop = fasst_next_hop(_hop, _step);
  _frame++;
  return FASST_FRAME_US;
}

// FASSTest. The callback's steps: a hop's packet; after a link packet or
// a request, the RX on (RX), then its packets read (READ)
enum { ST_LINK, ST_LINK_RX, ST_LINK_READ, ST_DATA, ST_REPLY_RX, ST_REPLY_READ };
enum { SERIAL_UNKNOWN, SERIAL_CANDIDATE, SERIAL_KNOWN };

static struct {
  bool ch12;
  uint8_t chips;   // per bit
  uint8_t len;     // packet bytes
  uint16_t frame_us;
  uint8_t id[FASSTEST_ID_LEN];
  uint8_t code, row;
  uint8_t step;
  uint32_t n;      // hops since the start
  uint8_t i;       // hop index in the row
  uint8_t lc;      // link channel
  uint8_t fs_ch;
  uint8_t serial[4], serial_state, misses;
  uint8_t pages, page, probe;  // pages answered with something; next ones
  uint32_t requests;
} _ft;

static uint8_t _rx_bytes[FASSTEST_PACKET_LEN];

// Offsets from a hop's start (its callback's deadline): the TX packet's
// end + 500 us, the RX on; its reply read when its RX would have ended
static uint32_t _rx_on_us(uint8_t chips, uint8_t len)
{
  return FASST_TX_DELAY_US + (80 + 8 * len) * chips * 2 / 3 + 500;
}

static uint32_t _read_us(uint8_t chips, uint8_t len, uint32_t reply_us)
{
  return FASST_TX_DELAY_US + reply_us + FASST_RX_BITS(chips, len) * chips * 2 / 3 +
         FASSTEST_RX_MARGIN_US;
}

static void _tx(const uint8_t* pkt, uint8_t chips, uint8_t len)
{
  fasst_spread_n(_chips, chips, pkt, len);
  radio_set_sync_word(fasst_sync_word(_chips), fasst_rx_sync_word(chips));
  radio_tx_start_at(fasst_chips(_chips), (80 + 8 * len) * chips / 8 - 4,
                    mpm_callback_deadline() + FASST_TX_DELAY_US);
}

// The next request's page (18CH)
static uint8_t _next_page()
{
  if (_ft.requests % FASSTEST_PROBE_EVERY == FASSTEST_PROBE_EVERY - 1 && _ft.pages != 0x0F) {
    do _ft.probe = (_ft.probe + 1) % FASSTEST_PAGES;
    while (_ft.pages >> _ft.probe & 1);
    return _ft.probe;
  }
  do _ft.page = (_ft.page + 1) % FASSTEST_PAGES;
  while (!(_ft.pages >> _ft.page & 1));
  return _ft.page;
}

// A reply heard on channel c: the receiver's serial, then to the radio
static void _reply(uint8_t c, int8_t rssi)
{
  uint8_t plain[FASSTEST_PACKET_LEN], cand[4];
  if (_ft.serial_state != SERIAL_UNKNOWN &&
      fasstest_reply_unpack(_rx_bytes, _ft.ch12, plain, _ft.serial, _ft.id, c)) {
    _ft.serial_state = SERIAL_KNOWN;
    _ft.misses = 0;
  } else {
    if (_ft.serial_state == SERIAL_KNOWN && ++_ft.misses < FASSTEST_SERIAL_MISSES) return;
    _ft.serial_state = SERIAL_UNKNOWN;
    if (!fasstest_reply_serial(_rx_bytes, _ft.ch12, _ft.id, c, cand)) return;
    memcpy(_ft.serial, cand, sizeof(cand));
    // 18CH: checked by the CRC; 12CH: solved from it, the next reply checks
    _ft.serial_state = _ft.ch12 ? SERIAL_CANDIDATE : SERIAL_KNOWN;
    if (_ft.ch12) return;
    fasstest_reply_unpack(_rx_bytes, false, plain, _ft.serial, _ft.id, c);
  }

  // To the radio: the page (12CH: 0x80), the reply's RSSI here, the data
  uint8_t n = _ft.len - 4, d[2 + FASSTEST_PACKET_LEN - 4];
  uint8_t page = _ft.ch12 ? 0 : plain[1] >> 4 & 3;
  d[0] = _ft.ch12 ? 0x80 : page;
  d[1] = (uint8_t)rssi;
  memcpy(d + 2, plain + 2, n);
  for (int i = 0; i < n; i++)
    if (plain[2 + i]) _ft.pages |= 1 << page;
  mpm_telem_send(MPM_TELEM_FUTABA, d, 2 + n);
}

static void _ft_init(bool ch12)
{
  memset(&_ft, 0, sizeof(_ft));
  _ft.ch12 = ch12;
  _ft.chips = ch12 ? FASST_BARKER_7 : FASST_BARKER_11;
  _ft.len = ch12 ? FASSTEST_PACKET_LEN_12CH : FASSTEST_PACKET_LEN;
  _ft.frame_us = ch12 ? FASSTEST_FRAME_US_12CH : FASSTEST_FRAME_US;
  uint32_t id = 100000000 + mpm.id % 900000000;
  for (int i = 0; i < 4; i++) _ft.id[i] = id >> (24 - 8 * i);
  _ft.row = id % 23;
  _ft.code = fasstest_code(0, _ft.row);
  _ft.pages = 1;
  _ft.step = mpm.bind_in_progress ? ST_LINK : ST_DATA;
}

// Bind: a link packet on each link channel in turn, FASSTEST_LINK_GAP hops
// apart, every FASSTEST_LINK_EVERY hops, 15 ms hops (both modes); the hop
// after one listened to. The reply (or the radio ending the bind): the data
// 2 hops after the link packet
static uint16_t _ft_link()
{
  if (!mpm.bind_in_progress) {
    _ft.step = ST_DATA;
    return FASSTEST_FRAME_US;
  }
  uint32_t i = _ft.n++ % FASSTEST_LINK_EVERY;
  if (i % FASSTEST_LINK_GAP || i / FASSTEST_LINK_GAP >= FASSTEST_LINK_CHANNELS)
    return FASSTEST_FRAME_US;
  _ft.lc = FASSTEST_LINK_CHANNEL(i / FASSTEST_LINK_GAP);
  radio_set_frequency_start(fasstest_freq_khz(_ft.lc));
  uint8_t pkt[FASSTEST_PACKET_LEN];
  fasstest_link_packet(pkt, _ft.ch12, _ft.id, _ft.code, _ft.lc, FASSTEST_BFS_DEFAULT);
  _tx(pkt, FASST_BARKER_11, FASSTEST_PACKET_LEN);
  _ft.step = ST_LINK_RX;
  return _rx_on_us(FASST_BARKER_11, FASSTEST_PACKET_LEN);
}

static uint16_t _ft_link_read()
{
  while (radio_receive(&_pkt))
    if (fasst_rx_bytes(_pkt.data, FASST_BARKER_11, FASSTEST_PACKET_LEN, _rx_bytes, NULL) >= 0 &&
        (_rx_bytes[0] & 0x1C) == FASSTEST_TYPE_LINK_REPLY &&
        !memcmp(_rx_bytes + 2, _ft.id, FASSTEST_ID_LEN)) {
      memcpy(_ft.serial, _rx_bytes + 10, sizeof(_ft.serial));
      _ft.serial_state = SERIAL_KNOWN;
      mpm_bind_done();
      _ft.step = ST_DATA;
    }
  radio_rx_stop();
  _ft.n++;
  if (_ft.step != ST_DATA) _ft.step = ST_LINK;
  return 2 * FASSTEST_FRAME_US - _read_us(FASST_BARKER_11, FASSTEST_PACKET_LEN,
                                          FASSTEST_LINK_REPLY_US);
}

static uint16_t _ft_data()
{
  _update_power();
  uint8_t c = fasstest_channel(0, _ft.row, _ft.i);
  radio_set_frequency_start(fasstest_freq_khz(c));
  uint16_t v[FASSTEST_TX_CHANNELS];
  for (int i = 0; i < FASSTEST_TX_CHANNELS; i++) v[i] = _value(mpm.channels[i]);

  uint8_t pkt[FASSTEST_PACKET_LEN], req = 0;
  uint32_t req_every = _ft.ch12 ? FASSTEST_REQ_EVERY_12CH : FASSTEST_REQ_EVERY;
  uint8_t n_ch = _ft.ch12 ? FASSTEST_TX_CHANNELS_12CH : FASSTEST_TX_CHANNELS;
  if (!mpm.disable_telem && _ft.n % req_every == 0) {
    req = 1 + (_ft.ch12 ? 0 : _next_page());
    _ft.requests++;
    fasstest_packet(pkt, _ft.ch12, _ft.id, _ft.code, c, v, _ft.n & 1, req);
  } else if (mpm.failsafe_valid && _ft.n % FASSTEST_FS_EVERY == 1) {
    uint16_t f = mpm.failsafe[_ft.fs_ch];
    bool hold = f == MPM_FAILSAFE_HOLD || f == MPM_FAILSAFE_NO_PULSE;
    fasstest_fs_packet(pkt, _ft.ch12, _ft.id, _ft.code, c, v, _ft.fs_ch,
                       hold ? FASSTEST_FS_HOLD : FASSTEST_FS_FS, hold ? v[_ft.fs_ch] : _value(f));
    _ft.fs_ch = (_ft.fs_ch + 1) % n_ch;
  } else {
    fasstest_packet(pkt, _ft.ch12, _ft.id, _ft.code, c, v, _ft.n & 1, 0);
  }
  _tx(pkt, _ft.chips, _ft.len);
  _ft.n++;
  _ft.i = (_ft.i + 1) % FASSTEST_HOPS;
  if (!req) return _ft.frame_us;
  _ft.step = ST_REPLY_RX;
  return _rx_on_us(_ft.chips, _ft.len);
}

static uint32_t _reply_us() { return _ft.ch12 ? FASSTEST_REPLY_US_12CH : FASSTEST_REPLY_US; }

// The reply in the next hop's slot, on its channel
static uint16_t _ft_reply_rx()
{
  radio_set_frequency(fasstest_freq_khz(fasstest_channel(0, _ft.row, _ft.i)));
  radio_rx_start(FASST_RX_PKT_LEN(_ft.chips, _ft.len));
  _ft.step = ST_REPLY_READ;
  return _read_us(_ft.chips, _ft.len, _reply_us()) - _rx_on_us(_ft.chips, _ft.len);
}

static uint16_t _ft_reply_read()
{
  uint8_t c = fasstest_channel(0, _ft.row, _ft.i);
  while (radio_receive(&_pkt))
    if (fasst_rx_bytes(_pkt.data, _ft.chips, _ft.len, _rx_bytes, NULL) >= 0 &&
        (_rx_bytes[0] & 0x1C) == FASSTEST_TYPE_REPLY)
      _reply(c, _pkt.rssi);
  radio_rx_stop();
  _ft.n++;
  _ft.i = (_ft.i + 1) % FASSTEST_HOPS;
  _ft.step = ST_DATA;
  return 2 * _ft.frame_us - _read_us(_ft.chips, _ft.len, _reply_us());
}

static uint16_t _ft_callback()
{
  switch (_ft.step) {
    case ST_LINK:
      return _ft_link();
    case ST_LINK_RX:
      radio_rx_start(FASST_RX_PKT_LEN(FASST_BARKER_11, FASSTEST_PACKET_LEN));
      _ft.step = ST_LINK_READ;
      return _read_us(FASST_BARKER_11, FASSTEST_PACKET_LEN, FASSTEST_LINK_REPLY_US) -
             _rx_on_us(FASST_BARKER_11, FASSTEST_PACKET_LEN);
    case ST_LINK_READ:
      return _ft_link_read();
    case ST_REPLY_RX:
      return _ft_reply_rx();
    case ST_REPLY_READ:
      return _ft_reply_read();
    default:
      return _ft_data();
  }
}

static bool _ft_mode() { return mpm.sub_protocol == SUB_FT18 || mpm.sub_protocol == SUB_FT12; }

void FASST_init()
{
  if (mpm.sub_protocol > SUB_FT12) {
    mpm_sub_invalid();
    return;
  }
  _power = _tx_power();
  radio_phy_t phy;
  fasst_phy(&phy, _power);
  if (_ft_mode()) {
    _ft_init(mpm.sub_protocol == SUB_FT12);
    radio_set_frequency(fasstest_freq_khz(fasstest_channel(0, _ft.row, 0)));
  } else {
    fasst_set_id(FASST_ID_BYTE0 << 24 | (mpm.id & 0xFFFFFF));
    _hop = FASST_HOP_FIRST;
    _frame = 0;
    _listening = true;
    _heard = 0;
    radio_set_frequency(fasst_freq_khz(FASST_LISTEN_HOP));
    radio_set_sync_word(0, fasst_rx_sync_word(FASST_BARKER_11));
  }
  if (radio_init(&phy) != RADIO_OK) mpm_sub_invalid();
  _listen_start = micros();
  if (mpm.bind_in_progress && !_ft_mode()) mpm_bind_done();
}

uint16_t FASST_callback() { return _ft_mode() ? _ft_callback() : _fasst_callback(); }
#endif
