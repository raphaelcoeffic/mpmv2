// Jeti Duplex follower (bench_jeti.h, doc/jeti.md)
#include "bench_jeti.h"
#include "jeti_frame.h"
#include "radio.h"
#include "timer.h"

#define JETI_LIST (JETI_HDR + 5)
#define JETI_DATA_LEN 37    // module frame with channel data (42 unbound)
#define JETI_REPLY_LEN 480  // air bytes for a reply (up to ~54 B PSDU: JETIBOX text)
// The receiver's reply comes 2.8 to 3.4 ms after the module frame, up to
// 1.9 ms long
#define JETI_REPLY_END_US 6000
// The hop clock: one ring step per 10 ms; frames within -3.5..+4.7 ms of a
// step's mean frame time, the bits of byte 5 telling where (mean offsets
// below: bits 1 late, 3 early). Step boundaries 5.6 ms after the mean
#define JETI_STEP_US 10000
#define JETI_BOUNDARY_US 5600
#define JETI_RESYNC_US 2500
#define JETI_LOST_US 300000
#define JETI_PARK_US 600000
#define JETI_BURST_MAX 400
static const int16_t _bits_offset_us[4] = {0, 2600, 500, -1200};

static radio_packet_t _pkt;
static uint8_t _burst[JETI_BURST_MAX];
static uint8_t _psdu[JETI_PSDU_MAX];

static uint32_t _ch_khz(uint8_t ch) { return jeti_freq_khz(ch - 11); }

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
  bool fcs = got == len && jeti_fcs_ok(_psdu, len);
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
  radio_phy_t phy;
  jeti_phy(&phy, 0);
  // Noise: an LCG's bytes
  uint32_t r = 12345;
  for (int i = 0; i < JETI_BURST_MAX; i++) _burst[i] = (r = r * 1103515245u + 12345) >> 24;
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
      int got = jeti_despread(_pkt.data, _pkt.len, _psdu, &len, &pre, &dmax);
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
        jeti_descramble(_psdu, pl, sizeof(pl));
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
        if (cfg->burst_at && module % cfg->burst_every == 0) {
          radio_status_t st = radio_tx_start_at(_burst, cfg->burst_len, t_sfd + cfg->burst_at);
          if (st == RADIO_OK) st = radio_tx_wait();
          print("@jb %lu %d\r\n", t_sfd, st);
        }
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
