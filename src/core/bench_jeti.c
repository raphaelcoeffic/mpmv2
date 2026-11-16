// Jeti Duplex follower (bench_jeti.h, doc/jeti.md)
#include "bench_jeti.h"
#include "platform.h"
#include "radio.h"
#include "timer.h"

// 802.15.4 symbol 0 in MSK form (the RX sync word: one preamble symbol)
#define JETI_SYNC 0xE077AE6C
// The 16 symbols' chip sequences in MSK form at 2 Mchip/s, first chip in bit
// 31; that bit depends on the previous symbol's last chip: left out
static const uint32_t _msk[16] = {
    0x6077AE6C, 0x4E077AE6, 0x6CE077AE, 0x66CE077A, 0x2E6CE077, 0x7AE6CE07,
    0x77AE6CE0, 0x077AE6CE, 0x1F885193, 0x31F88519, 0x131F8851, 0x1931F885,
    0x51931F88, 0x051931F8, 0x0851931F, 0x78851931,
};
#define JETI_MSK_MASK 0x7FFFFFFF
#define JETI_SYMBOL_US 16
#define JETI_PREAMBLE_SYMBOLS 8
#define JETI_PSDU_MAX 127

#define JETI_HDR 7         // MAC header: FCF, seq, PAN, dst
#define JETI_TYPE JETI_HDR  // payload byte 0, plain
#define JETI_LIST (JETI_HDR + 5)
#define JETI_DATA_LEN 37    // module frame with channel data (42 unbound)
#define JETI_REPLY_LEN 200  // air bytes for a reply (17 B PSDU)
// The receiver's reply comes 2.8 to 3.4 ms after the module frame
#define JETI_REPLY_END_US 4000
// The hop clock: one ring step per 10 ms; frames within -3.5..+4.7 ms of a
// step's mean frame time, the bits of byte 5 telling where (mean offsets
// below: bits 1 late, 3 early). Step boundaries 5.6 ms after the mean
#define JETI_STEP_US 10000
#define JETI_BOUNDARY_US 5600
#define JETI_RESYNC_US 2500
#define JETI_LOST_US 300000
#define JETI_PARK_US 600000
static const int16_t _bits_offset_us[4] = {0, 2600, 500, -1200};

static radio_packet_t _pkt;
static uint8_t _psdu[JETI_PSDU_MAX];

static uint32_t _ch_khz(uint8_t ch) { return 2405000 + 5000 * (ch - 11); }

static uint8_t _symbol(const uint8_t* d, uint8_t* dist)
{
  uint32_t w = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
  uint8_t best = 0, bd = 32;
  for (uint8_t s = 0; s < 16; s++) {
    uint8_t n = __builtin_popcount((w ^ _msk[s]) & JETI_MSK_MASK);
    if (n < bd) {
      bd = n;
      best = s;
    }
  }
  *dist = bd;
  return best;
}

// Despread the capture after the sync word: the preamble's remaining
// symbols, the SFD (7, A), the PHR, the PSDU. Returns the PSDU bytes the
// capture holds, -1 without an SFD; *len: the PHR's length, *pre: the
// preamble symbols after the sync word, *dmax: the worst symbol distance
static int _despread(const uint8_t* d, uint16_t n, uint8_t* len, uint8_t* pre, uint8_t* dmax)
{
  uint16_t syms = n / 4, i = 0;
  uint8_t dist, s;
  *dmax = 0;
  while (i < syms && i < JETI_PREAMBLE_SYMBOLS && (s = _symbol(d + 4 * i, &dist)) == 0) i++;
  *pre = i;
  if (i + 4 > syms || s != 7 || _symbol(d + 4 * (i + 1), &dist) != 0xA) return -1;
  i += 2;
  uint8_t d0, d1;
  uint8_t lo = _symbol(d + 4 * i, &d0), hi = _symbol(d + 4 * (i + 1), &d1);
  *len = (lo | hi << 4) & 0x7F;
  *dmax = d0 > d1 ? d0 : d1;
  i += 2;
  int k = 0;
  for (; k < *len && i + 2 <= syms; k++, i += 2) {
    lo = _symbol(d + 4 * i, &d0);
    hi = _symbol(d + 4 * (i + 1), &d1);
    _psdu[k] = lo | hi << 4;
    if (d0 > *dmax) *dmax = d0;
    if (d1 > *dmax) *dmax = d1;
  }
  return k;
}

static uint16_t _crc16_kermit(const uint8_t* p, uint8_t n)
{
  uint16_t c = 0;
  while (n--) {
    c ^= *p++;
    for (int b = 0; b < 8; b++) c = c & 1 ? (c >> 1) ^ 0x8408 : c >> 1;
  }
  return c;
}

// Payload scrambler (doc/jeti/jdec.py): key register fed with every
// plaintext byte, payload bytes from 1 on sent as p ^ k
static void _descramble(const uint8_t* in, uint8_t* out, uint8_t n)
{
  uint8_t k = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t p = i <= JETI_TYPE ? in[i] : in[i] ^ k;
    out[i] = p;
    k = ((uint8_t)((k << 1) + p) ^ (k & 0x80 ? 0xC8 : 0)) ^ 0x93;
  }
}

static void _print_frame(jeti_print_t print, uint32_t t_sfd, uint8_t ch, int got, uint8_t len,
                         uint8_t dmax, int off)
{
  static const char digits[] = "0123456789abcdef";
  char hex[2 * JETI_PSDU_MAX + 1];
  for (int i = 0; i < got; i++) {
    hex[2 * i] = digits[_psdu[i] >> 4];
    hex[2 * i + 1] = digits[_psdu[i] & 0xF];
  }
  hex[2 * got] = 0;
  bool fcs = got == len && len >= 2 &&
             _crc16_kermit(_psdu, len - 2) == (_psdu[len - 2] | _psdu[len - 1] << 8);
  print("@jf %lu %u rssi=%d d=%u fcs=%u ", t_sfd, ch, _pkt.rssi, dmax, fcs);
  print("%s", hex);
  if (off != INT16_MIN) print(" off=%d", off);
  print("\r\n");
}

static void _rx_on(uint8_t ch, uint16_t len)
{
  radio_rx_stop();
  radio_set_frequency(_ch_khz(ch));
  radio_rx_start(len);
}

void jeti_rx_run(const jeti_rx_cfg_t* cfg, jeti_print_t print)
{
  radio_phy_t phy = {
      .symbol_rate = 2000000,
      .deviation = 500000,
      .rx_bw = 100,
      .preamble_bytes = 4,
      .sync_bits = 32,
      .if_freq = (int16_t)0x8000,
      .ble_2m = RADIO_2M_BLE,  // CC2652: 2 Mbps only there
  };
  uint8_t c = cfg->ch;
  bool ok = radio_init(&phy) == RADIO_OK;
  radio_set_sync_word(JETI_SYNC, JETI_SYNC);
  radio_set_rx_sync_alt(0);
  if (!ok || radio_set_frequency(_ch_khz(c)) != RADIO_OK ||
      radio_rx_start(cfg->frame_len) != RADIO_OK) {
    print("@err jetirx radio status=0x%04x\r\n", radio_last_status());
    radio_off();
    return;
  }

  // Locked: the ring of 5 channels (the last data frame's list, its channel
  // in slot 4), the current step's slot si, the next step boundary t_bound,
  // m_ref a step's mean frame time (the phase); after a module frame on
  // its channel until t_reply_end (the reply), then on the step's channel
  bool locked = false, reply = false;
  uint8_t ring[5] = {0}, si = 0;
  uint32_t m_ref = 0, t_bound = 0, t_reply_end = 0, t_frame = 0, t_park = micros();
  uint32_t frames = 0, module = 0, replies = 0, bad = 0, locks = 0, lost = 0, resyncs = 0;
  uint32_t restarts = 0;
  uint32_t t_end = micros() + cfg->secs * 1000000;
  while ((cfg->secs == 0 || micros_before(t_end)) && !(cfg->stop && cfg->stop())) {
    radio_poll();
    if (radio_receive(&_pkt)) {
      uint8_t len, pre, dmax;
      int got = _despread(_pkt.data, _pkt.len, &len, &pre, &dmax);
      if (got < 0) {
        bad++;
        continue;
      }
      frames++;
      uint32_t t_sfd = _pkt.timestamp_us + JETI_SYMBOL_US * pre;
      uint8_t type = got > JETI_TYPE ? _psdu[JETI_TYPE] & 0xF : 0;
      int off = INT16_MIN;
      if (type == 2) replies++;
      if (type == 1 && got >= JETI_LIST + 3 && !reply) {
        module++;
        uint8_t pl[JETI_LIST + 3];
        _descramble(_psdu, pl, sizeof(pl));
        uint8_t list[5] = {pl[JETI_LIST] & 0xF, pl[JETI_LIST + 1] >> 4, pl[JETI_LIST + 1] & 0xF,
                           pl[JETI_LIST + 2] >> 4, pl[JETI_LIST + 2] & 0xF};
        uint32_t m = t_sfd - _bits_offset_us[(pl[JETI_HDR + 5] >> 5) & 3];
        if (cfg->follow && len >= JETI_DATA_LEN && list[4] + 11 == c) {
          if (locked) {
            uint32_t k = (m - m_ref + JETI_STEP_US / 2) / JETI_STEP_US;
            int32_t err = (int32_t)(m - (m_ref + k * JETI_STEP_US));
            off = err;
            if (err > JETI_RESYNC_US || err < -JETI_RESYNC_US) {
              resyncs++;
              m_ref = m;
            } else {
              m_ref += k * JETI_STEP_US + err / 4;
            }
          } else {
            locks++;
            m_ref = m;
          }
          locked = true;
          for (int j = 0; j < 5; j++) ring[j] = list[j] + 11;
          si = 4;
          t_bound = m_ref + JETI_BOUNDARY_US;
          if ((int32_t)(t_bound - t_sfd) < 0) t_bound += JETI_STEP_US;
        }
        t_frame = micros();
        t_park = t_frame;
        t_reply_end = t_sfd + JETI_REPLY_END_US;
        reply = true;
        radio_rx_stop();
        radio_rx_start(JETI_REPLY_LEN);
      }
      _print_frame(print, t_sfd, c, got, len, dmax, off);
    } else if (locked && !micros_before(t_bound)) {
      // Next step: its channel, unless on a frame's channel for the reply
      si = (si + 1) % 5;
      t_bound += JETI_STEP_US;
      if (!reply) {
        c = ring[si];
        _rx_on(c, cfg->frame_len);
      }
    } else if (reply && !micros_before(t_reply_end)) {
      reply = false;
      if (locked) c = ring[si];
      _rx_on(c, cfg->frame_len);
    } else if (locked && micros() - t_frame > JETI_LOST_US) {
      lost++;
      locked = false;
      c = cfg->ch;
      t_park = micros();
      print("@jlost %lu %u\r\n", t_park, c);
      _rx_on(c, cfg->frame_len);
    } else if (!locked && !reply && c != cfg->ch && !micros_before(t_park + JETI_PARK_US)) {
      c = cfg->ch;
      t_park = micros();
      print("@jpark %lu %u\r\n", t_park, c);
      _rx_on(c, cfg->frame_len);
    } else if (!radio_rx_active()) {
      restarts++;
      radio_rx_start(reply ? JETI_REPLY_LEN : cfg->frame_len);
    }
  }
  radio_rx_stats_t st;
  radio_rx_stats(&st);
  radio_off();
  print("@end jetirx frames=%lu module=%lu replies=%lu bad=%lu locks=%lu lost=%lu resyncs=%lu "
        "restarts=%lu buf_full=%u\r\n",
        frames, module, replies, bad, locks, lost, resyncs, restarts, st.buf_full);
}
