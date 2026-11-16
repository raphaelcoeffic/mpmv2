// Jeti Duplex 2.4 GHz (jeti_frame.h, doc/jeti.md), as the Jeti TU-style
// PPM module (both AT86RF230): 8 channels, one frame per 20 ms, the
// receiver's basic telemetry.
//
// - Addresses: the transmitter's own PAN and short address from the module
//   id + rx_num (one receiver binding per model); the receiver's, learned
//   at bind, saved per model (JETI_EEPROM_OFFSET). Data frames go to the
//   receiver's address (802.15.4 broadcast if none saved).
// - Hopping: a ring of 5 channels (IEEE 11..26; first from the id), one
//   step per 10 ms; the frames carry the ring rotated so the current
//   step's channel is last, the next step's first, and byte 5 bits 5-6 the
//   frame's place in its step (2: the middle, where ours are). One frame
//   every 2 steps: all 5 channels in turn.
// - Channel sweep, adaptive as the module's (doc/jeti.md, jam test): every
//   JETI_SWEEP_FRAMES frames one slot moves to the next channel above its
//   own not in the ring (wrapping) and not marked bad: the one with the
//   most frames unanswered since it moved, else the one unmoved longest
//   (with a clean link: all 16 channels in turn). A channel is bad for
//   JETI_BAD_TICKS ticks after a frame there was skipped (busy) or not
//   answered: a slot jumps over a busy stretch (Wi-Fi: ~4 channels) in one
//   move instead of a tick per channel, as the module does. All bad: the
//   next not in the ring. Never the current frame's slot nor the
//   next frame's: the next frame is on a channel of both rings, for a
//   receiver that missed the change.
// - Carrier sense, as the receiver's before its replies (doc/jeti.md, burst
//   test): RX on the frame's channel from JETI_CCA_AT_US, the RSSI's
//   maximum over JETI_CCA_US (802.15.4's 8 symbols); at JETI_CCA_DBM and
//   above the frame is skipped (no random back-off: the frame must stay in
//   its step), counted as unanswered for the sweep, which moves its slot
//   at the next tick. TX LQI: the frames sent in % (last JETI_LQI_FRAMES).
// - Bind (the receiver in pairing: bind plug in Ext at power-up, or the
//   JETIBOX's Pairing): the receiver hops on its last transmitter's
//   channels, which we don't know: we stay JETI_BIND_DWELL steps on each
//   channel in turn, a frame every step (one of 5 lands in the receiver's
//   step on that channel if its ring has it), each frame saying "all 5
//   steps here". Per channel the module's boot: 5 frames 0x23 (our PAN and
//   short), then 0x22 ones with channel data, to the broadcast address. The
//   receiver answers a 0x22 one with its address appended; it then keeps
//   only us (it holds one transmitter). The data start at once, the ring's
//   current slot set to the bind channel (the receiver waits there).
// - Telemetry: the receiver answers each frame 2.8..3.4 ms later on its
//   channel (17 bytes): a frame counter, every 4th reply its voltage (10
//   mV), the active antenna, a signal level. To the radio as the FrSky
//   link frame: A1 = the voltage (255 = 13.2 V), RX RSSI = the signal
//   level byte, TX RSSI = the reply's RSSI here + 120 dBm, RX LQI = replies
//   in % of frames (last JETI_LQI_FRAMES). And each reply as it came
//   (MPM_TELEM_JETI, kind 0).
// - Remote JETIBOX (the receiver's own menus, as the module relays them):
//   while the radio's frames carry a byte (MPM_TELEM_JETI: buttons LDUR in
//   bits 7-4, 1 = pressed, a press counter in bits 3-0), every
//   JETI_BOX_EVERY-th data frame gets `44 <JETIBOX byte>` (0xFF: none; a
//   new counter value: its buttons in the next 2 of them, as the module),
//   and the receiver answers it with its screen: to the radio
//   (MPM_TELEM_JETI, kind 1, 32 characters).
//
// Channels: +-100% (MPM_CHANNEL_MIN..MAX) = 1000..2000 us; slot value
// 8 x (us - 1500) - 1, 14 bits.
#include "protocols.h"

#ifdef JETI_INO
#include <string.h>

#include "jeti_frame.h"
#include "mpm_compat.h"  // rf_power_word(), eeprom, link telemetry
#undef sub_protocol      // its MULTI-Module name: mpm.sub_protocol here
#include "radio.h"
#include "timer.h"

#define JETI_STEP_US 10000
#define JETI_RING 5
#define JETI_FRAME_STEPS 2
// TX start in the step: the SFD ~150 us later at 4.9 ms, the module's
// mean frame time for byte 5 bits 5-6 = 2
#define JETI_TX_AT_US 4750
#define JETI_PLACE_MIDDLE 2
#define JETI_SFD_US (JETI_TX_AT_US + 150)
// Carrier sense before it: RX on, the first valid RSSI, JETI_CCA_US of
// samples (given up at JETI_CCA_MAX_US: clear), RX off, TX at JETI_TX_AT_US
#define JETI_CCA_AT_US (JETI_TX_AT_US - 450)
#define JETI_CCA_US 128
#define JETI_CCA_MAX_US 330
#define JETI_CCA_DBM (-70)  // EN 300 328's LBT threshold at 20 dBm, per MHz
// The reply comes 2.8..3.4 ms after our SFD, on its channel; read (from the
// step's start) when its capture has ended: data, up to a ~54 byte reply
// (JETIBOX text, 1.9 ms); bind, a 22 byte one, within the step (a frame
// every step). Fixed length captures (the CC2652's Bluetooth LE PHY)
#define JETI_READ_US (JETI_SFD_US + 6000)
#define JETI_RX_LEN 480
#define JETI_BIND_READ_US (JETI_SFD_US + 5000)
#define JETI_BIND_RX_LEN 240
#define JETI_BIND_DWELL 15      // steps per channel at bind
#define JETI_BIND_BOOT 5        // 0x23 frames first on each channel
#define JETI_LQI_FRAMES 50
#define JETI_SWEEP_FRAMES 12  // 240 ms (the module: a 250 ms tick)
#define JETI_BAD_TICKS 40     // ~10 s
#define JETI_BOX_EVERY 3     // the module: every 3rd frame (~67 ms)
#define JETI_BOX_FRAMES 10   // the terminal closed after this many frames without its byte
#define JETI_BOX_PRESS 2     // a press in this many JETIBOX frames
#define JETI_BOX_PAYLOAD (JETI_DATA_PAYLOAD + 2)
#define JETI_SCREEN_LEN 32
#define JETI_ID_BYTE 0x58       // after the short address, as the module's

// Payload bytes (after the MAC header)
#define P_TYPE 0
#define P_SUB 1
#define JETI_TYPE_TX 0xB1
#define JETI_DATA_PAYLOAD 28   // 8 channels
#define JETI_BOOT_PAYLOAD 12   // 0x23
#define JETI_BIND_PAYLOAD 30   // 0x22, 8 channels
#define JETI_CH 8

#define JETI_EEPROM_OFFSET 1792  // mpm-v2's own: RX PAN, short (LE), 4 bytes per model

enum { ST_BIND_TX, ST_BIND_RX, ST_BIND_READ, ST_TX, ST_CCA, ST_RX, ST_READ };

static struct {
  uint8_t st;
  uint16_t pan, short_addr;        // ours
  uint16_t rx_pan, rx_short;       // the receiver's (0xFFFF: broadcast)
  uint8_t ring[JETI_RING];
  uint8_t si;                      // current step's ring slot
  // Sweep: per slot the frames unanswered and the frame count when it
  // moved; the slot of the last data frame
  uint8_t miss[JETI_RING];
  uint32_t moved[JETI_RING];
  uint8_t tx_slot;
  uint8_t bad[JETI_CHANNELS];      // sweep ticks left
  uint8_t seq;
  uint32_t n;                      // frames
  uint8_t bind_ch, dwell;
  uint16_t power;
  uint8_t lqi_frames, lqi_replies, lqi_sent;
  uint16_t air_len, payload_len;  // the next data frame's
  uint16_t rx_on;                  // from the step's start: RX on
  // Remote JETIBOX: open (frames with its byte lately), the last press
  // counter, the press being sent and its frames left
  bool box;
  uint8_t box_idle, box_count, box_press, box_press_n;
} _j;

static uint8_t _frame[JETI_HDR + JETI_BOX_PAYLOAD];
static uint8_t _air[JETI_TX_LEN(JETI_HDR + JETI_BIND_PAYLOAD + JETI_FCS)];
static uint8_t _psdu[JETI_PSDU_MAX], _plain[JETI_PSDU_MAX];
static radio_packet_t _pkt;

static uint16_t _tx_power() { return rf_power_word(mpm.range_check ? 34 : mpm.low_power ? 9 : 0); }

static void _update_power()
{
  uint16_t p = _tx_power();
  if (p != _j.power) {
    _j.power = p;
    radio_set_tx_power(p);
  }
}

// 1/8 us from 1500 us, minus 1, 14 bits
static uint16_t _slot(uint16_t ch)
{
  int32_t v = ((int32_t)ch - (MPM_CHANNEL_MIN + MPM_CHANNEL_MAX) / 2) * 8000 /
                  (MPM_CHANNEL_MAX - MPM_CHANNEL_MIN) - 1;
  if (v < -8192) v = -8192;
  if (v > 8191) v = 8191;
  return v & 0x3FFF;
}

static uint8_t* _slots(uint8_t* p)
{
  for (int i = 0; i < JETI_CH; i++) {
    uint16_t v = _slot(mpm.channels[i]);
    *p++ = v;
    *p++ = v >> 8;
  }
  return p;
}

// The ring rotated (current step's channel last), byte 5 with the place
static uint8_t* _list(uint8_t* p, const uint8_t* ring, uint8_t si)
{
  uint8_t l[JETI_RING];
  for (int j = 0; j < JETI_RING; j++) l[j] = ring[(si + 1 + j) % JETI_RING];
  *p++ = JETI_PLACE_MIDDLE << 5 | l[0];
  *p++ = l[1] << 4 | l[2];
  *p++ = l[3] << 4 | l[4];
  return p;
}

// The frame on the air: _air, its length returned
static uint16_t _prepare(uint8_t payload_len)
{
  uint8_t len = JETI_HDR + payload_len;
  jeti_scramble(_frame, _frame, len);
  _j.n++;
  return jeti_spread(_air, _frame, len);
}

static void _send(uint8_t payload_len)
{
  radio_tx_start_at(_air, _prepare(payload_len), mpm_callback_deadline() + JETI_TX_AT_US);
}

// Carrier sense on the tuned channel: false if busy
static bool _clear()
{
  radio_rx_start(JETI_RX_LEN);
  uint32_t t0 = micros(), first = 0;
  bool valid = false;
  int8_t max = RADIO_RSSI_NONE;
  while (micros() - t0 < JETI_CCA_MAX_US) {
    int8_t r = radio_rssi();
    if (r == RADIO_RSSI_NONE) continue;
    if (!valid) {
      valid = true;
      first = micros();
    }
    if (r > max) max = r;
    if (micros() - first >= JETI_CCA_US) break;
  }
  radio_rx_stop();
  return max < JETI_CCA_DBM;  // no valid RSSI (RADIO_RSSI_NONE): clear
}

// From the step's start: our TX's end (preamble, sync word, 4 us per air
// byte), then the RX on
static uint16_t _rx_on_us(uint8_t payload_len)
{
  _j.rx_on = JETI_TX_AT_US + 32 + JETI_TX_LEN(JETI_HDR + payload_len + JETI_FCS) * 4 + 100;
  return _j.rx_on;
}

// A reply to us: its payload in _plain, its length returned (0: none)
static uint8_t _reply()
{
  uint8_t len, pre, dmax;
  int got = jeti_despread(_pkt.data, _pkt.len, _psdu, &len, &pre, &dmax);
  if (got != len || len < JETI_HDR + 8 + JETI_FCS || !jeti_fcs_ok(_psdu, len)) return 0;
  len -= JETI_FCS;
  jeti_descramble(_psdu, _plain, len);
  const uint8_t* h = _plain;
  if ((h[JETI_TYPE] & 0xF) != 2 || h[JETI_TYPE + P_SUB] != 0x13 ||
      (h[3] | h[4] << 8) != _j.pan || (h[5] | h[6] << 8) != _j.short_addr)
    return 0;
  memmove(_plain, _plain + JETI_HDR, len - JETI_HDR);
  return len - JETI_HDR;
}

// Reply payload: 0 type, 1 0x13, 2-3 A (frame counter), 4-5 B (bits 14-15
// 01: the voltage, 10 mV), 6 flags (bits 0-1 antenna, 2 bind plug, 7 link
// new), 7 signal level; then 0x22 + the receiver's PAN and short (pairing),
// or 0x43 + 32 JETIBOX characters
static void _telemetry(int8_t rssi)
{
  uint16_t b = _plain[4] | _plain[5] << 8;
  if ((b & 0xC000) == 0x4000) {
    uint32_t v = (b & 0x3FFF) * 255 / 1320;
    v_lipo1 = v > 255 ? 255 : v;
  }
  RX_RSSI = _plain[7];
  int t = rssi + 120;
  TX_RSSI = t < 0 ? 0 : t > 127 ? 127 : t;
  telemetry_link |= 1;
}

static void _lqi(bool sent, bool reply)
{
  _j.lqi_sent += sent;
  _j.lqi_replies += reply;
  if (++_j.lqi_frames >= JETI_LQI_FRAMES) {
    RX_LQI = _j.lqi_replies * 100 / JETI_LQI_FRAMES;
    TX_LQI = _j.lqi_sent * 100 / JETI_LQI_FRAMES;
    _j.lqi_frames = _j.lqi_replies = _j.lqi_sent = 0;
  }
}

static void _miss()
{
  if (_j.miss[_j.tx_slot] < 0xFF) _j.miss[_j.tx_slot]++;
  _j.bad[_j.ring[_j.tx_slot]] = JETI_BAD_TICKS;
}

static void _load_rx()
{
  uint16_t a = JETI_EEPROM_OFFSET + 4 * mpm.rx_num;
  _j.rx_pan = eeprom_read_byte(a) | eeprom_read_byte(a + 1) << 8;
  _j.rx_short = eeprom_read_byte(a + 2) | eeprom_read_byte(a + 3) << 8;
}

static void _save_rx()
{
  uint16_t a = JETI_EEPROM_OFFSET + 4 * mpm.rx_num;
  eeprom_write_byte(a, _j.rx_pan);
  eeprom_write_byte(a + 1, _j.rx_pan >> 8);
  eeprom_write_byte(a + 2, _j.rx_short);
  eeprom_write_byte(a + 3, _j.rx_short >> 8);
}

// Bind: a frame every step on the bind channel, all 5 steps there
static uint16_t _bind_tx()
{
  if (!mpm.bind_in_progress) {
    _j.st = ST_TX;
    return JETI_STEP_US;
  }
  _update_power();
  radio_set_frequency_start(jeti_freq_khz(_j.bind_ch));
  uint8_t ring[JETI_RING];
  memset(ring, _j.bind_ch, sizeof(ring));
  jeti_header(_frame, _j.seq++, 0xFFFF, 0xFFFF);
  uint8_t* p = _frame + JETI_HDR;
  bool boot = _j.dwell < JETI_BIND_BOOT;
  *p++ = JETI_TYPE_TX;
  if (boot) {
    *p++ = 0x23;
    *p++ = 0x01;
    *p++ = 0x00;
  } else {
    *p++ = 0x22;
  }
  *p++ = _j.pan;
  *p++ = _j.pan >> 8;
  *p++ = _j.short_addr;
  *p++ = _j.short_addr >> 8;
  *p++ = JETI_ID_BYTE;
  p = _list(p, ring, 0);
  if (!boot) {
    *p++ = 0x11;
    *p++ = 0x80;
    *p++ = 0x00;
    *p++ = 0x37;
    p = _slots(p);
  }
  uint8_t pl = p - (_frame + JETI_HDR);
  _send(pl);
  _j.st = ST_BIND_RX;
  return _rx_on_us(pl);
}

static uint16_t _bind_read()
{
  const uint16_t elapsed = JETI_BIND_READ_US;
  while (radio_receive(&_pkt)) {
    uint8_t n = _reply();
    if (n >= 13 && _plain[8] == 0x22) {
      _j.rx_pan = _plain[9] | _plain[10] << 8;
      _j.rx_short = _plain[11] | _plain[12] << 8;
      _save_rx();
      // The receiver waits on the bind channel: there in our ring, at the
      // next step's slot (the first data frame's), swapped with the slot
      // that had it
      _j.si = (_j.si + 1) % JETI_RING;
      for (int j = 0; j < JETI_RING; j++)
        if (_j.ring[j] == _j.bind_ch) _j.ring[j] = _j.ring[_j.si];
      _j.ring[_j.si] = _j.bind_ch;
      radio_rx_stop();
      mpm_bind_done();
      _j.st = ST_TX;
      return JETI_STEP_US - elapsed;
    }
  }
  radio_rx_stop();
  _j.si = (_j.si + 1) % JETI_RING;
  if (++_j.dwell >= JETI_BIND_DWELL) {
    _j.dwell = 0;
    _j.bind_ch = (_j.bind_ch + 1) % JETI_CHANNELS;
  }
  _j.st = ST_BIND_TX;
  return JETI_STEP_US - elapsed;
}

// One slot to the next free channel above its own
static void _sweep()
{
  uint8_t next = (_j.si + JETI_FRAME_STEPS) % JETI_RING, best = 0xFF;
  for (uint8_t j = 0; j < JETI_RING; j++) {
    if (j == _j.si || j == next) continue;
    if (best == 0xFF || _j.miss[j] > _j.miss[best] ||
        (_j.miss[j] == _j.miss[best] && _j.moved[j] < _j.moved[best]))
      best = j;
  }
  // The next channel above not in the ring: a good one, else any
  uint8_t c = _j.ring[best], any = 0xFF;
  for (uint8_t k = 1; k < JETI_CHANNELS; k++) {
    uint8_t t = (c + k) % JETI_CHANNELS, j;
    for (j = 0; j < JETI_RING && _j.ring[j] != t; j++) {}
    if (j < JETI_RING) continue;
    if (any == 0xFF) any = t;
    if (!_j.bad[t]) {
      c = t;
      break;
    }
  }
  if (c == _j.ring[best]) c = any;
  _j.ring[best] = c;
  for (uint8_t k = 0; k < JETI_CHANNELS; k++)
    if (_j.bad[k]) _j.bad[k]--;
  _j.miss[best] = 0;
  _j.moved[best] = _j.n;
}

// The radio's JETIBOX byte (the last frame's data): the terminal open, a
// new press. Whether this data frame carries the JETIBOX byte
static bool _box_frame()
{
  if (mpm.data_len >= 1) {
    uint8_t d = mpm.data[0];
    if (!_j.box) {
      _j.box = true;
      _j.box_count = d & 0x0F;  // a press before: not sent
    } else if ((d & 0x0F) != _j.box_count) {
      _j.box_count = d & 0x0F;
      _j.box_press = d & 0xF0;
      _j.box_press_n = JETI_BOX_PRESS;
    }
    _j.box_idle = 0;
  } else if (_j.box && ++_j.box_idle >= JETI_BOX_FRAMES) {
    _j.box = false;
    _j.box_press_n = 0;
  }
  return _j.box && _j.n % JETI_BOX_EVERY == 0;
}

// A reply to the radio (MPM_TELEM_JETI): kind 0, the reply's RSSI here
// and its bytes 2..7; with a screen, kind 1 and its 32 characters
static void _telem_jeti(uint8_t n, int8_t rssi)
{
  uint8_t d[1 + JETI_SCREEN_LEN];
  d[0] = MPM_JETI_REPLY;
  d[1] = (uint8_t)rssi;
  memcpy(d + 2, _plain + 2, 6);
  mpm_telem_send(MPM_TELEM_JETI, d, 8);
  if (n >= 9 + JETI_SCREEN_LEN && _plain[8] == 0x43) {
    d[0] = MPM_JETI_SCREEN;
    memcpy(d + 1, _plain + 9, JETI_SCREEN_LEN);
    mpm_telem_send(MPM_TELEM_JETI, d, sizeof(d));
  }
}

// Data: a frame every 2 steps on the current step's channel
static uint16_t _data_tx()
{
  if (mpm.bind_in_progress) {
    _j.st = ST_BIND_TX;
    _j.dwell = 0;
    return JETI_STEP_US;
  }
  _update_power();
  if (_j.n % JETI_SWEEP_FRAMES == 0) _sweep();
  _j.tx_slot = _j.si;
  radio_set_frequency_start(jeti_freq_khz(_j.ring[_j.si]));
  jeti_header(_frame, _j.seq++, _j.rx_pan, _j.rx_short);
  uint8_t* p = _frame + JETI_HDR;
  *p++ = JETI_TYPE_TX;
  *p++ = 0x21;
  *p++ = _j.short_addr;
  *p++ = _j.short_addr >> 8;
  *p++ = JETI_ID_BYTE;
  p = _list(p, _j.ring, _j.si);
  *p++ = 0x11;
  *p++ = (_j.n & 3) << 6;  // a 2-bit frame counter, as the module's
  *p++ = 0xB6;             // unknown, the module's 0xa7..0xc9
  *p++ = 0x37;
  p = _slots(p);
  if (_box_frame()) {
    *p++ = 0x44;
    uint8_t b = 0xFF;
    if (_j.box_press_n) {
      b ^= _j.box_press;
      _j.box_press_n--;
    }
    *p++ = b;
  }
  _j.payload_len = p - (_frame + JETI_HDR);
  _j.air_len = _prepare(_j.payload_len);
  _j.si = (_j.si + JETI_FRAME_STEPS) % JETI_RING;
  _j.st = ST_CCA;
  return JETI_CCA_AT_US;
}

// The data frame, if its channel is clear
static uint16_t _data_cca()
{
  if (!_clear()) {
    _lqi(false, false);
    _miss();
    _j.st = ST_TX;
    return JETI_FRAME_STEPS * JETI_STEP_US - JETI_CCA_AT_US;
  }
  radio_tx_start_at(_air, _j.air_len,
                    mpm_callback_deadline() + JETI_TX_AT_US - JETI_CCA_AT_US);
  if (mpm.disable_telem) {
    _j.st = ST_TX;
    return JETI_FRAME_STEPS * JETI_STEP_US - JETI_CCA_AT_US;
  }
  _j.st = ST_RX;
  return _rx_on_us(_j.payload_len) - JETI_CCA_AT_US;
}

static uint16_t _data_read()
{
  bool got = false;
  uint8_t n;
  while (radio_receive(&_pkt))
    if ((n = _reply())) {
      got = true;
      _telemetry(_pkt.rssi);
      _telem_jeti(n, _pkt.rssi);
    }
  radio_rx_stop();
  _lqi(true, got);
  if (!got) _miss();
  _j.st = ST_TX;
  return JETI_FRAME_STEPS * JETI_STEP_US - JETI_READ_US;
}

void JETI_init()
{
  memset(&_j, 0, sizeof(_j));
  // Ours: never 0x0000 / 0xFFFF (802.15.4 reserved), PAN 0xAxxx as Jeti's
  _j.pan = 0xA000 | ((mpm.id >> 16) & 0x0FFF);
  _j.short_addr = mpm.id & 0xFFFF;
  if (_j.short_addr == 0 || _j.short_addr >= 0xFFFE) _j.short_addr ^= 0x5A5A;
  // The ring: 5 different channels from the id
  uint32_t r = mpm.id * 2654435761u;
  for (int k = 0; k < JETI_RING;) {
    r = r * 1103515245u + 12345;
    uint8_t c = r >> 20 & 0xF, j;
    for (j = 0; j < k && _j.ring[j] != c; j++) {}
    if (j == k) _j.ring[k++] = c;
  }
  _load_rx();
  _j.power = _tx_power();
  radio_phy_t phy;
  jeti_phy(&phy, _j.power);
  if (radio_init(&phy) != RADIO_OK) {
    mpm_sub_invalid();
    return;
  }
  radio_set_sync_word(JETI_SYNC, JETI_SYNC);
  radio_set_rx_sync_alt(0);
  radio_set_frequency(jeti_freq_khz(_j.ring[0]));
  _j.st = mpm.bind_in_progress ? ST_BIND_TX : ST_TX;
}

uint16_t JETI_callback()
{
  switch (_j.st) {
    case ST_BIND_TX:
      return _bind_tx();
    case ST_BIND_RX:
      radio_rx_start(JETI_BIND_RX_LEN);
      _j.st = ST_BIND_READ;
      return JETI_BIND_READ_US - _j.rx_on;
    case ST_BIND_READ:
      return _bind_read();
    case ST_RX:
      radio_rx_start(JETI_RX_LEN);
      _j.st = ST_READ;
      return JETI_READ_US - _j.rx_on;
    case ST_CCA:
      return _data_cca();
    case ST_READ:
      return _data_read();
    default:
      return _data_tx();
  }
}
#endif
