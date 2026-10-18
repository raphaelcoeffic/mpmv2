#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "board.h"
#include "frame.h"
#include "radio.h"
#include "timer.h"
#include "uart.h"

// Commands (one per line, numbers in decimal or 0x hex):
//
//   info                         @ok bench=1 dev=cc2652 ...
//   preset <nrf1m|nrf2m|nrf250k|xn250k>  load a PHY preset
//   phy <rate> <dev_hz> <rx_bw> [preamble_bytes] [if_freq]   custom 2-GFSK PHY
//   pwr <word>                   RF core TX power word (applied by next preset/phy)
//   syncthr <n>                  sync correlation threshold, 0 = auto: default
//                                (0x1F), strict (0x32) for weak RX sync words
//   sync <hex>                   raw framing: sync word, 1 to 4 bytes
//   nrf <addr> [crc_bytes] [sync_bytes]  NRF24L01 framing, addr as written to
//                                TX_ADDR (LSByte first), 3 to 5 bytes, crc 0..2
//                                (default 2), address bytes used as sync word 2..4
//                                (default 4)
//   xn <crc> <scramble> <250k> [force_nrf] [enhanced]  XN297 framing and PHY
//                                (force_nrf: ignored, for MULTI-Module scripts)
//   xaddr <hex>                  XN297 address (as for XN297_SetTXAddr), 3..5 bytes
//   prefix [hex]                 known first payload bytes (none to clear): the RX
//                                sync word moves into them, see frame.h
//   esb <0|1> [dpl] [noack]      NRF Enhanced ShockBurst: packet control field, as
//                                sent by the nRF24L01+ with EN_AA or DYNPD set
//   ack <0|1> [retries] [window_us] [ack_max_len]  wait for an acknowledgment
//                                after each packet, as the nRF24L01+ with EN_AA:
//                                RX window after each TX, retransmissions
//   ch <n>                       channel: 2400 + n MHz (NRF numbering)
//   freq <khz>                   frequency in kHz
//   tx <hex>                     send one packet
//   txp <period_us> <count> <hex>  send periodically, count 0 = forever
//   seq <idx>                    8 bit counter at byte idx for txp, 255 = off
//   rx <len>                     receive continuously, len = payload size
//                                (XN297 enhanced: maximum size)
//   stop                         stop the current job
//
// Events: @rx <time_us> <ch> <hex> [crc=ok|bad] rssi=<dBm> [pid=<n>]
//         @ack <time_us> <ch> <hex> tries=<n> rssi=<dBm> (ack payloads)
//         @ev txdone <n>

#define BENCH_BAUDRATE 921600
#define LINE_MAX_LEN 200
#define SEQ_OFF 255

typedef enum { JOB_IDLE, JOB_TXP, JOB_RX } job_t;

static char _line[LINE_MAX_LEN + 1];
static uint32_t _line_len;

static radio_phy_t _phy;
static char _phy_name[16] = "none";
static bool _phy_ok;
static uint16_t _tx_power = 0x7217;  // TI default for 2.4 GHz prop (+5 dBm)
static uint8_t _sync_threshold;
#define SYNC_THRESHOLD_STRICT 0x32

static uint32_t _freq_khz = 2440000;
static int _channel = 40;

// TI default sync word
static frame_t _frame = {
    .type = FRAME_RAW,
    .addr = {0x93, 0x0B, 0x51, 0xDE},
    .addr_len = 4,
};
static bool _frame_ok;

static job_t _job;
static uint8_t _tx_buf[FRAME_PAYLOAD_MAX];
static uint8_t _tx_len;
static uint32_t _tx_period;
static uint32_t _tx_count;
static uint32_t _tx_next;
static uint8_t _seq_idx = SEQ_OFF;
static uint8_t _seq;
static uint8_t _rx_len;

static uint32_t _stat_tx;
static uint32_t _stat_rx;
static uint32_t _stat_err;

// Acknowledgments (ESB): same framing as the data, empty or with a payload
// (dynamic payload length only). As the nRF24L01+, any valid ack is accepted:
// its packet id is not checked (it differs on acks of retransmissions).
static bool _ack_on;
static uint8_t _ack_retries = 3;
static uint32_t _ack_window = 500;
static uint8_t _ack_max_len = FRAME_PAYLOAD_MAX;
static frame_t _ack_frame;
static uint32_t _stat_acked;
static uint32_t _stat_retries;

static const char* const _frame_names[] = {"raw", "nrf", "xn297"};

//
// Output
//

static void _print(const char* str) { uart_print(UART0, str); }

static void _printf(const char* fmt, ...)
{
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  _print(buf);
}

static void _print_hex(const uint8_t* data, uint32_t len)
{
  static const char digits[] = "0123456789abcdef";
  for (uint32_t i = 0; i < len; i++) {
    uart_put_char_blocking(UART0, digits[data[i] >> 4]);
    uart_put_char_blocking(UART0, digits[data[i] & 0xF]);
  }
}

//
// Argument parsing
//

static char* _next_arg(char** args)
{
  char* p = *args;
  while (*p == ' ' || *p == '\t') p++;
  if (!*p) return NULL;

  char* start = p;
  while (*p && *p != ' ' && *p != '\t') p++;
  if (*p) *p++ = 0;
  *args = p;
  return start;
}

static bool _number(char** args, uint32_t* value)
{
  char* arg = _next_arg(args);
  if (!arg) return false;

  char* end;
  *value = strtoul(arg, &end, 0);
  return *end == 0;
}

static uint32_t _number_or(char** args, uint32_t def)
{
  uint32_t value;
  return _number(args, &value) ? value : def;
}

static int _hex_digit(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Rest of the line as bytes: "0a1b", "0a 1b" or "0x0a 0x1b". Returns -1 on error.
static int _hex(char* args, uint8_t* buf, uint32_t size)
{
  uint32_t len = 0;
  char* arg;
  while ((arg = _next_arg(&args))) {
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) arg += 2;
    uint32_t n = strlen(arg);
    if (n == 0 || n & 1) return -1;
    for (uint32_t i = 0; i < n; i += 2) {
      int hi = _hex_digit(arg[i]);
      int lo = _hex_digit(arg[i + 1]);
      if (hi < 0 || lo < 0 || len >= size) return -1;
      buf[len++] = (hi << 4) | lo;
    }
  }
  return len;
}

//
// Radio helpers
//

static void _set_sync_word()
{
  radio_set_sync_word(frame_tx_sync(&_frame), frame_rx_sync(&_frame));
}

static bool _apply_phy()
{
  _phy.sync_bits = frame_sync_bits(&_frame);
  _phy.tx_power = _tx_power;
  // Measured at 1 Mbps: 0x32 instead of 0x1F gets address 6565656565 from 50%
  // to 100%. Its sensitivity cost is not measured: only used when needed.
  _phy.sync_threshold = _sync_threshold ? _sync_threshold
                        : frame_rx_sync_weak(&_frame) ? SYNC_THRESHOLD_STRICT : 0;
  _phy_ok = radio_init(&_phy) == RADIO_OK && radio_set_frequency(_freq_khz) == RADIO_OK;
  _set_sync_word();
  return _phy_ok;
}

static void _report_radio_error(const char* what)
{
  _printf("@err %s status=0x%04x\r\n", what, radio_last_status());
}

static void _job_stop()
{
  if (_job == JOB_RX) radio_rx_stop();
  _job = JOB_IDLE;
}

// Applies a framing change: the sync word size is part of the PHY setup
static bool _apply_frame()
{
  _job_stop();
  _frame_ok = frame_init(&_frame);
  if (!_frame_ok) return false;
  _ack_frame = _frame;
  _ack_on = _ack_on && _frame.type == FRAME_NRF && _frame.esb && frame_init(&_ack_frame);
  if (_phy_ok && !_apply_phy()) {
    _report_radio_error("radio init");
    return false;
  }
  return true;
}

typedef struct {
  bool acked;
  uint8_t tries;
  radio_packet_t pkt;
  uint8_t payload[FRAME_PAYLOAD_MAX];
  uint8_t len;
  uint16_t pcf;
} ack_t;

// Sends and waits for the ack, up to 1 + _ack_retries times
static radio_status_t _send_acked(const uint8_t* air, uint8_t n, ack_t* ack)
{
  uint8_t max_len = _ack_frame.dpl ? _ack_max_len : 0;
  uint8_t rx_len = frame_rx_len(&_ack_frame, max_len);
  for (ack->tries = 1; ack->tries <= 1 + _ack_retries; ack->tries++) {
    radio_status_t st = radio_send_rx(air, n, frame_rx_sync(&_ack_frame), rx_len, _ack_window,
                                      &ack->pkt);
    if (st == RADIO_NO_PACKET) continue;
    if (st != RADIO_OK) return st;
    if (frame_decode_pcf(&_ack_frame, ack->pkt.data, ack->pkt.len, max_len, ack->payload,
                         &ack->len, &ack->pcf) == FRAME_OK) {
      ack->acked = true;
      return RADIO_OK;
    }
  }
  ack->tries--;
  return RADIO_OK;
}

static bool _send_packet(const uint8_t* payload, uint8_t len, ack_t* ack)
{
  uint8_t air[RADIO_MAX_PKT_LEN];
  uint8_t n = frame_tx_data(&_frame, payload, len, air);
  radio_status_t st = RADIO_ERR_PARAM;
  if (n) st = _ack_on ? _send_acked(air, n, ack) : radio_send(air, n);
  if (st != RADIO_OK) {
    _stat_err++;
    return false;
  }
  _stat_tx++;
  if (_ack_on) {
    _stat_retries += ack->tries - 1;
    if (ack->acked) _stat_acked++;
  }
  return true;
}

static void _report_packet(const radio_packet_t* pkt)
{
  uint8_t payload[FRAME_PAYLOAD_MAX];
  uint8_t len;
  uint16_t pcf;
  frame_result_t res =
      frame_decode_pcf(&_frame, pkt->data, pkt->len, _rx_len, payload, &len, &pcf);

  _printf("@rx %lu %d ", pkt->timestamp_us, _channel);
  _print_hex(payload, len);
  if (_frame.crc_len) _printf(" crc=%s", res == FRAME_OK ? "ok" : "bad");
  _printf(" rssi=%d", pkt->rssi);
  if (_frame.type == FRAME_NRF && _frame.esb) _printf(" pid=%u", (pcf >> 1) & 3);
  _print("\r\n");
}

static void _report_ack(const ack_t* ack)
{
  _printf("@ack %lu %d ", ack->pkt.timestamp_us, _channel);
  _print_hex(ack->payload, ack->len);
  _printf(" tries=%u rssi=%d\r\n", ack->tries, ack->pkt.rssi);
}

static bool _rx_start()
{
  return radio_rx_start(frame_rx_len(&_frame, _rx_len)) == RADIO_OK;
}

//
// Commands
//

static bool _need_phy()
{
  if (!_phy_ok) _print("@err no phy: use preset or phy first\r\n");
  if (_phy_ok && !_frame_ok) _print("@err framing not configured (address?)\r\n");
  return _phy_ok && _frame_ok;
}

static void _cmd_info()
{
  _printf("@ok bench=1 dev=cc2652 phy=%s rate=%lu dev_hz=%lu rx_bw=%u ch=%d freq=%lu "
          "framing=%s esb=%d ack=%d\r\n",
          _phy_name, _phy.symbol_rate, _phy.deviation, _phy.rx_bw, _channel, _freq_khz,
          _frame_names[_frame.type], _frame.type == FRAME_NRF && _frame.esb, _ack_on);
}

static bool _load_preset(const char* name)
{
  // nRF24L01: 160 kHz deviation at 250k/1M, 320 kHz at 2M.
  // XN297 at 250 kbps: as the MULTI-Module CC2500 emulation (127 kHz)
  radio_phy_t phy = {
      .preamble_bytes = 4,
      .if_freq = 0x8000,  // RF core default
  };
  if (!strcmp(name, "nrf1m")) {
    phy.symbol_rate = 1000000;
    phy.deviation = 160000;
    phy.rx_bw = 97;  // 1243 kHz
  } else if (!strcmp(name, "nrf2m")) {
    phy.symbol_rate = 2000000;
    phy.deviation = 320000;
    phy.rx_bw = 100;  // 2185 kHz
  } else if (!strcmp(name, "nrf250k")) {
    phy.symbol_rate = 250000;
    phy.deviation = 160000;
    phy.rx_bw = 94;  // 784 kHz
  } else if (!strcmp(name, "xn250k")) {
    phy.symbol_rate = 250000;
    phy.deviation = 127000;
    phy.rx_bw = 94;  // 784 kHz
  } else {
    _printf("@err unknown preset %s\r\n", name);
    return false;
  }

  _job_stop();
  _phy = phy;
  strncpy(_phy_name, name, sizeof(_phy_name) - 1);
  if (!_apply_phy()) {
    _report_radio_error("radio init");
    return false;
  }
  return true;
}

static void _cmd_preset(char* args)
{
  char* name = _next_arg(&args);
  if (!name) {
    _print("@err usage: preset <nrf1m|nrf2m|nrf250k|xn250k>\r\n");
    return;
  }
  if (_load_preset(name)) _printf("@ok preset=%s\r\n", name);
}

static void _cmd_phy(char* args)
{
  uint32_t rate, dev, bw;
  if (!_number(&args, &rate) || !_number(&args, &dev) || !_number(&args, &bw)) {
    _print("@err usage: phy <rate> <dev_hz> <rx_bw> [preamble_bytes] [if_freq]\r\n");
    return;
  }

  _job_stop();
  _phy.symbol_rate = rate;
  _phy.deviation = dev;
  _phy.rx_bw = bw;
  _phy.preamble_bytes = _number_or(&args, 4);
  _phy.if_freq = _number_or(&args, 0x8000);
  strcpy(_phy_name, "custom");
  if (!_apply_phy()) {
    _report_radio_error("radio init");
    return;
  }
  _print("@ok phy=custom\r\n");
}

static void _cmd_syncthr(char* args)
{
  uint32_t thr;
  if (_number(&args, &thr) && thr < 0x80) _sync_threshold = thr;
  _printf("@ok syncthr=0x%02x\r\n", _sync_threshold);
}

static void _cmd_pwr(char* args)
{
  uint32_t pwr;
  if (_number(&args, &pwr)) _tx_power = pwr;
  _printf("@ok pwr=0x%04x\r\n", _tx_power);
}

static void _cmd_sync(char* args)
{
  uint8_t buf[4];
  int len = _hex(args, buf, sizeof(buf));
  if (len <= 0) {
    _print("@err usage: sync <hex>, 1 to 4 bytes\r\n");
    return;
  }

  _frame.type = FRAME_RAW;
  memcpy(_frame.addr, buf, len);
  _frame.addr_len = len;
  _frame.prefix_len = 0;
  if (!_apply_frame()) return;
  _printf("@ok framing=raw sync=%08lx bits=%u\r\n", frame_tx_sync(&_frame),
          frame_sync_bits(&_frame));
}

static void _cmd_nrf(char* args)
{
  char* addr = _next_arg(&args);
  uint8_t buf[FRAME_ADDR_MAX];
  int len = addr ? _hex(addr, buf, sizeof(buf)) : -1;
  uint32_t crc = _number_or(&args, 2);
  uint32_t sync = _number_or(&args, 4);
  if (len < 3 || crc > 2 || sync < 2 || sync > 4) {
    _print("@err usage: nrf <addr: 3..5 bytes, TX_ADDR order> [crc_bytes 0..2] "
           "[sync_bytes 2..4]\r\n");
    return;
  }

  _frame.type = FRAME_NRF;
  _frame.esb = false;
  memcpy(_frame.addr, buf, len);
  _frame.addr_len = len;
  _frame.crc_len = crc;
  _frame.sync_bytes = sync;
  if (!_apply_frame()) {
    if (!_frame_ok) _print("@err bad nrf framing\r\n");
    return;
  }
  _printf("@ok framing=nrf addr_len=%d crc=%lu sync=%u\r\n", len, crc, _frame.sync_len);
}

static void _cmd_xn(char* args)
{
  uint32_t crc, scramble, rate_250k;
  if (!_number(&args, &crc) || !_number(&args, &scramble) || !_number(&args, &rate_250k)) {
    _print("@err usage: xn <crc> <scramble> <250k> [force_nrf] [enhanced]\r\n");
    return;
  }
  _number_or(&args, 0);  // force_nrf
  uint32_t enhanced = _number_or(&args, 0);

  _frame.type = FRAME_XN297;
  _frame.crc_len = crc ? 2 : 0;
  _frame.scramble = scramble;
  _frame.enhanced = enhanced;
  _frame.pid = 0;
  if (enhanced) _frame.prefix_len = 0;
  if (!_load_preset(rate_250k ? "xn250k" : "nrf1m")) return;
  // Valid once the address is set
  _apply_frame();
  _printf("@ok xn297 rate=%s enhanced=%lu\r\n", rate_250k ? "250k" : "1m", enhanced);
}

static void _cmd_xaddr(char* args)
{
  uint8_t buf[FRAME_ADDR_MAX];
  int len = _hex(args, buf, sizeof(buf));
  if (_frame.type != FRAME_XN297 || len < 3) {
    _print("@err usage: xn first, then xaddr <3..5 bytes>\r\n");
    return;
  }
  memcpy(_frame.addr, buf, len);
  _frame.addr_len = len;
  if (!_apply_frame()) {
    if (!_frame_ok) _print("@err bad xn297 framing\r\n");
    return;
  }
  _printf("@ok xaddr len=%d\r\n", len);
}

static void _cmd_prefix(char* args)
{
  uint8_t buf[FRAME_PREFIX_MAX];
  int len = _hex(args, buf, sizeof(buf));
  if (len < 0) {
    _printf("@err usage: prefix [hex], up to %d bytes\r\n", FRAME_PREFIX_MAX);
    return;
  }
  memcpy(_frame.prefix, buf, len);
  _frame.prefix_len = len;
  if (!_apply_frame()) {
    if (!_frame_ok) _print("@err prefix not supported with this framing\r\n");
    return;
  }
  _printf("@ok prefix=%d\r\n", len);
}

static void _cmd_esb(char* args)
{
  uint32_t on;
  if (_frame.type != FRAME_NRF || !_number(&args, &on)) {
    _print("@err usage: nrf first, then esb <0|1> [dpl] [noack]\r\n");
    return;
  }
  _frame.esb = on;
  _frame.dpl = _number_or(&args, 1);
  _frame.noack = _number_or(&args, 0);
  _frame.pid = 0;
  if (!_apply_frame()) {
    if (!_frame_ok) _print("@err bad esb framing (prefix?)\r\n");
    return;
  }
  _printf("@ok esb=%lu dpl=%u noack=%u\r\n", on, _frame.dpl, _frame.noack);
}

static void _cmd_ack(char* args)
{
  uint32_t on;
  if (!_number(&args, &on)) {
    _print("@err usage: ack <0|1> [retries] [window_us] [ack_max_len]\r\n");
    return;
  }
  uint32_t retries = _number_or(&args, _ack_retries);
  uint32_t window = _number_or(&args, _ack_window);
  uint32_t max_len = _number_or(&args, _ack_max_len);
  if (on && (_frame.type != FRAME_NRF || !_frame.esb)) {
    _print("@err ack needs esb framing\r\n");
    return;
  }
  if (retries > 15 || window < 100 || window > 100000 || max_len > FRAME_PAYLOAD_MAX) {
    _print("@err bad ack parameters\r\n");
    return;
  }
  _job_stop();
  _ack_retries = retries;
  _ack_window = window;
  _ack_max_len = max_len;
  _ack_on = on;
  _printf("@ok ack=%lu retries=%u window=%lu max_len=%u\r\n", on, _ack_retries, _ack_window,
          _ack_max_len);
}

static bool _set_frequency(uint32_t khz)
{
  job_t job = _job;
  if (job == JOB_RX) radio_rx_stop();

  _freq_khz = khz;
  bool ok = !_phy_ok || radio_set_frequency(khz) == RADIO_OK;
  if (ok && job == JOB_RX) ok = _rx_start();
  if (!ok) {
    _job = JOB_IDLE;
    _report_radio_error("frequency");
  }
  return ok;
}

static void _cmd_ch(char* args)
{
  uint32_t ch;
  if (!_number(&args, &ch) || ch > 125) {
    _print("@err usage: ch <0..125>\r\n");
    return;
  }
  _channel = ch;
  if (_set_frequency(2400000 + ch * 1000)) _printf("@ok ch=%lu\r\n", ch);
}

static void _cmd_freq(char* args)
{
  uint32_t khz;
  if (!_number(&args, &khz) || khz < 2360000 || khz > 2500000) {
    _print("@err usage: freq <2360000..2500000 kHz>\r\n");
    return;
  }
  _channel = -1;
  if (_set_frequency(khz)) _printf("@ok freq=%lu\r\n", khz);
}

static bool _parse_payload(char* args)
{
  int len = _hex(args, _tx_buf, sizeof(_tx_buf));
  if (len <= 0 || !frame_len_ok(&_frame, len)) {
    _print("@err bad payload\r\n");
    return false;
  }
  _tx_len = len;
  return true;
}

static void _cmd_tx(char* args)
{
  if (!_need_phy() || !_parse_payload(args)) return;

  _job_stop();
  ack_t ack = {0};
  if (!_send_packet(_tx_buf, _tx_len, &ack)) {
    _report_radio_error("tx");
    return;
  }
  if (!_ack_on) {
    _print("@ok tx\r\n");
    return;
  }
  _printf("@ok tx pid=%u acked=%u tries=%u", _frame.tx_pid, ack.acked, ack.tries);
  if (ack.acked) {
    _printf(" ack_pcf=0x%03x ack_rssi=%d ack=", ack.pcf, ack.pkt.rssi);
    _print_hex(ack.payload, ack.len);
  } else if (ack.pkt.len) {
    _print(" last_rx=");  // last packet received in the ack window, if any
    _print_hex(ack.pkt.data, ack.pkt.len);
  }
  _print("\r\n");
}

static void _cmd_txp(char* args)
{
  uint32_t period, count;
  if (!_number(&args, &period) || !_number(&args, &count) || period < 500) {
    _print("@err usage: txp <period_us (>= 500)> <count> <hex>\r\n");
    return;
  }
  if (!_need_phy() || !_parse_payload(args)) return;

  _job_stop();
  _tx_period = period;
  _tx_count = count;
  _tx_next = micros();
  _seq = 0;
  _stat_tx = _stat_rx = _stat_err = _stat_acked = _stat_retries = 0;
  _job = JOB_TXP;
  _print("@ok txp\r\n");
}

static void _cmd_seq(char* args)
{
  uint32_t idx;
  if (!_number(&args, &idx) || idx > 255) {
    _print("@err usage: seq <idx|255>\r\n");
    return;
  }
  _seq_idx = idx;
  _printf("@ok seq=%lu\r\n", idx);
}

static void _cmd_rx(char* args)
{
  uint32_t len;
  if (!_number(&args, &len) || len == 0 || len > FRAME_PAYLOAD_MAX) {
    _print("@err usage: rx <payload_len>\r\n");
    return;
  }
  if (!_need_phy()) return;
  uint8_t air_len = frame_rx_len(&_frame, len);
  if (air_len == 0 || air_len > RADIO_MAX_PKT_LEN) {
    _print("@err bad rx length for this framing (prefix?)\r\n");
    return;
  }

  _job_stop();
  _rx_len = len;
  _stat_tx = _stat_rx = _stat_err = 0;
  if (!_rx_start()) {
    _report_radio_error("rx");
    return;
  }
  _job = JOB_RX;
  _printf("@ok rx len=%lu sync=%08lx syncthr=0x%02x\r\n", len, frame_rx_sync(&_frame),
          _phy.sync_threshold);
}

static void _cmd_stop()
{
  bool was_rx = _job == JOB_RX;
  _job_stop();
  _printf("@ok stop tx=%lu rx=%lu err=%lu", _stat_tx, _stat_rx, _stat_err);
  if (_ack_on) _printf(" acked=%lu retries=%lu", _stat_acked, _stat_retries);
  if (was_rx) {
    radio_rx_stats_t st;
    radio_rx_stats(&st);
    _printf(" rf_ok=%u rf_nok=%u rf_buf_full=%u", st.ok, st.nok, st.buf_full);
  }
  _print("\r\n");
}

static void _process_line(char* line)
{
  char* args = line;
  char* cmd = _next_arg(&args);
  if (!cmd) return;

  if (!strcmp(cmd, "info")) _cmd_info();
  else if (!strcmp(cmd, "preset")) _cmd_preset(args);
  else if (!strcmp(cmd, "phy")) _cmd_phy(args);
  else if (!strcmp(cmd, "pwr")) _cmd_pwr(args);
  else if (!strcmp(cmd, "syncthr")) _cmd_syncthr(args);
  else if (!strcmp(cmd, "sync")) _cmd_sync(args);
  else if (!strcmp(cmd, "nrf")) _cmd_nrf(args);
  else if (!strcmp(cmd, "xn")) _cmd_xn(args);
  else if (!strcmp(cmd, "xaddr")) _cmd_xaddr(args);
  else if (!strcmp(cmd, "prefix")) _cmd_prefix(args);
  else if (!strcmp(cmd, "esb")) _cmd_esb(args);
  else if (!strcmp(cmd, "ack")) _cmd_ack(args);
  else if (!strcmp(cmd, "ch")) _cmd_ch(args);
  else if (!strcmp(cmd, "freq")) _cmd_freq(args);
  else if (!strcmp(cmd, "tx")) _cmd_tx(args);
  else if (!strcmp(cmd, "txp")) _cmd_txp(args);
  else if (!strcmp(cmd, "seq")) _cmd_seq(args);
  else if (!strcmp(cmd, "rx")) _cmd_rx(args);
  else if (!strcmp(cmd, "stop")) _cmd_stop();
  else _printf("@err unknown command %s\r\n", cmd);
}

static void _poll_uart()
{
  while (uart_rx_fifo_available(UART0)) {
    char c = uart_get_char(UART0);
    if (c == '\r' || c == '\n') {
      _line[_line_len] = 0;
      // Strip comments, as in the host scripts
      char* comment = strchr(_line, '#');
      if (comment) *comment = 0;
      _process_line(_line);
      _line_len = 0;
    } else if (_line_len < LINE_MAX_LEN) {
      _line[_line_len++] = c;
    }
  }
}

//
// Jobs
//

static void _run_txp()
{
  if (micros_before(_tx_next)) return;
  _tx_next += _tx_period;

  if (_seq_idx < _tx_len) _tx_buf[_seq_idx] = _seq++;
  ack_t ack = {0};
  if (_send_packet(_tx_buf, _tx_len, &ack) && ack.acked && ack.len) _report_ack(&ack);

  if (_tx_count && _stat_tx + _stat_err >= _tx_count) {
    _job = JOB_IDLE;
    _printf("@ev txdone %lu\r\n", _stat_tx);
  }
}

static void _run_rx()
{
  radio_packet_t pkt;
  while (radio_receive(&pkt)) {
    _stat_rx++;
    _report_packet(&pkt);
  }

  // RX ends on errors (e.g. queue overflow): restart it
  if (!radio_rx_active()) {
    _stat_err++;
    if (!_rx_start()) {
      _job = JOB_IDLE;
      _report_radio_error("rx restart");
    }
  }
}

void bench_loop()
{
  uart_device_t dev = {
      .mode = UART_8N1,
      .baud_rate = BENCH_BAUDRATE,
      .rx = SERIAL_RX_IOD,
      .tx = SERIAL_TX_IOD,
      .cts = IOID_UNUSED,
      .rts = IOID_UNUSED,
  };
  uart_init(UART0, &dev);
  _frame_ok = frame_init(&_frame);
  _print("\r\n# mpm-v2 bench ready\r\n");

  while (true) {
    _poll_uart();

    switch (_job) {
      case JOB_TXP:
        _run_txp();
        break;
      case JOB_RX:
        _run_rx();
        break;
      default:
        break;
    }
  }
}
