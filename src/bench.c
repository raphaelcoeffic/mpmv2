#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>

#include "bench.h"
#include "board.h"
#include "frame.h"
#include "radio.h"
#include "sx1281.h"
#include "timer.h"
#include "uart.h"

#if defined(LED_SPI) && defined(LED_DIN)
  #include "led_rgb.h"
#endif

// Commands (one per line, numbers in decimal or 0x hex):
//
//   info                         @ok bench=1 dev=cc2652 ...
//   chip <cc2652|sx1281> [dbm]   radio for the PHY (applied at once and kept): the
//                                CC2652 RF core (default) or the SX1281 (MPM v2: GFSK
//                                125 kbps to 2 Mbps, TX power in dBm into the
//                                PA, default 0, max 2)
//   preset <name>                load a PHY preset (and its channel plan):
//                                nrf1m nrf2m nrf250k xn250k, CC2500 ones with
//                                their framing: frskyd frskyx frskyx2 frskyxeu
//                                frskyx2eu hott hitec redpine
//                                redpineslow skyartec frskyv corona coronav2
//                                fdv3 ares radiolink ikea sfhss, A7105 ones:
//                                flysky afhds2a hubsan bugs height pelikan
//                                kyosho kyoshohype wfly2 joysway, CYRF6936: dsm (data
//                                packets), dsmbind (bind packets: SDR), devo
//                                (32 chip SOP and data codes), gfsk1m (GFSK mode:
//                                PHY only, for hs6200 and rf2500), frskyl (FrSky L
//                                chip stream, prefix 4e: its first payload byte)
//   phy <rate> <dev_hz> <rx_bw> [preamble_bytes] [if_freq] [fsk]  custom PHY,
//                                2-GFSK or 2-FSK
//   pwr <word|default>           RF core TX power word (at once, SX1281: by the
//                                next preset/phy), default: RF_TX_POWER_DEFAULT
//                                (board.h)
//   syncthr <n>                  sync correlation threshold, 0 = auto: default
//                                (0x1F), strict (0x32) for weak RX sync words
//   ovr [word...]                up to 8 raw RF core override words added to
//                                the setup (register experiments), none = clear
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
//   cc2500 <sync> [crc] [var] [white] [fec] [msk]  CC2500 framing: sync word
//                                SYNC1 SYNC0 (twice for SYNC_MODE 3), CC2500 CRC
//                                (default 1, 2 = CC2400_EN), variable length
//                                (default 1): the
//                                payload starts with the length byte, as the
//                                CC2500 FIFO content. PN9 whitening, FEC, MSK
//                                bit inversion (default 0)
//   a7105 <id> [crc] [fec]       A7105 framing: ID (2 or 4 bytes, as given to
//                                A7105_WriteID), CRC (default 1), FEC (default 0)
//   cyrf <sop> <data_code> [seed] [crc] [len] [sdr] [data_chips] [invert]  CYRF6936
//                                framing: SOP code (8 bytes, 4 = 32 chip SOP,
//                                0 = no SOP) and data codes (16 bytes, SDR: 8)
//                                as given to CYRF_ConfigSOPCode() /
//                                CYRF_ConfigDataCode(), CRC seed (default 0),
//                                CRC16 (default 1), length field (default 1),
//                                SDR instead of 8DR (default 0), data code
//                                length 32 or 64 (default), all chips inverted
//                                (TX_INV, default 0). 8DR needs the SOP code,
//                                SDR 64 chips
//   hs6200 <addr> [crc] [scramble]  HS6200 framing (E01X): addr as given to
//                                HS6200_SetTXAddr(), 3 to 5 bytes, CRC and
//                                scrambling (default 1), no ack bit set
//   rf2500 <addr> [scramble] [crc]  RF2500 framing (E129: scrambled, 16 or 19
//                                bytes, E010R5: not): addr as given to
//                                RF2500_SetTXAddr(), 4 bytes, CRC (default 1)
//   esb <0|1> [dpl] [noack]      NRF Enhanced ShockBurst: packet control field, as
//                                sent by the nRF24L01+ with EN_AA or DYNPD set
//   ack <0|1> [retries] [window_us] [ack_max_len]  wait for an acknowledgment
//                                after each packet, as the nRF24L01+ with EN_AA:
//                                RX window after each TX, retransmissions
//   ch <n>                       channel: base + n * spacing of the channel plan
//                                (NRF: 2400 + n MHz)
//   chplan <base_hz> <spacing_hz>  channel plan (CC2500: FREQ and CHANSPC)
//   trim <khz>                   frequency correction added to ch and freq, signed
//                                (crystal offsets: the MULTI-Module CC2500 option)
//   peek <addr> [n] / poke <addr> <val>  read / write 32 bit words
//   autotrim [off | <offset_khz>]  trim follows the senders received: our
//                                frequency at theirs + offset_khz (default 0),
//                                from the modem's estimate (@rx foff=)
//   freq <khz>                   frequency in kHz
//   tx <hex>                     send one packet
//   txp <period_us> <count> <hex>  send periodically, count 0 = forever
//   seq <idx>                    8 bit counter at byte idx for txp, 255 = off
//   rx <len> [fixed]             receive continuously, len = payload size
//                                (length field: maximum size). With a length
//                                field (NRF dynamic payload, XN297 enhanced,
//                                CC2500 variable length), the RF core ends
//                                each packet at its length; fixed: receive
//                                the maximum length instead
//   stop                         stop the current job
//   sched <period_us> [n]        alarm chain on absolute deadlines (previous +
//                                period, timer_set_alarm()), n alarms (default
//                                100): @timing sched_late, callback start after
//                                its deadline. An rx job goes on meanwhile
//   timing [n] [len]             RF core latencies on the current PHY, framing
//                                and channel, n runs each (default 20), len
//                                byte payloads (default 16): @timing <what> n= min=
//                                avg= max= (us), @edges <what>: the PA (and
//                                LaunchPad: LNA) enable edges of the first run
//
// Events: @rx <time_us> <ch> <hex> [crc=ok|bad] rssi=<dBm> [foff=<khz>] [pid=<n>]
//           time_us: when the sync word was found (CC2652: RF core timer)
//           foff: sender's carrier above ours, estimate
//         @trim <khz> (autotrim changed the trim)
//         @ack <time_us> <ch> <hex> tries=<n> rssi=<dBm> (ack payloads)
//         @ev txdone <n>

#define BENCH_BAUDRATE 921600
#define LINE_MAX_LEN 300
#define SEQ_OFF 255
// TI specifies 2360 to 2500 MHz, Redpine hops up to 2502 MHz
#define FREQ_MAX_KHZ 2510000

typedef enum { JOB_IDLE, JOB_TXP, JOB_RX } job_t;

static char _line[LINE_MAX_LEN + 1];
static uint32_t _line_len;

static radio_phy_t _phy;
static char _phy_name[16] = "none";
static bool _phy_ok;
static uint16_t _tx_power = RF_TX_POWER_DEFAULT;
static uint8_t _sync_threshold;
static bool _sx;  // chip sx1281
static int8_t _sx_dbm = SX1281_TX_DBM_DEFAULT;
#define SYNC_THRESHOLD_STRICT 0x32

static uint32_t _freq_khz = 2440000;
static int _channel = 40;
static uint32_t _ch_base_hz = 2400000000;
static uint32_t _ch_spacing_hz = 1000000;
static int32_t _trim_khz;
// autotrim: mean of AUTOTRIM_PACKETS estimates, applied when it is
// AUTOTRIM_MIN_KHZ off (estimate steps of 0.5 to 4 kHz, noise of about 2).
// Scale errors of the estimate (+-20%) only add a step or two
#define AUTOTRIM_PACKETS 8
#define AUTOTRIM_MIN_KHZ 5
#define TRIM_MAX_KHZ 500
static bool _autotrim;
static int32_t _autotrim_offset;
static int32_t _autotrim_sum;
static uint8_t _autotrim_n;

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
static uint32_t _tx_late_max;  // us, worst txp packet start after its slot
static uint8_t _seq_idx = SEQ_OFF;
static uint8_t _seq;
static uint8_t _rx_len;
static bool _rx_fixed;  // the maximum length even with a length field

static uint32_t _stat_tx;
static uint32_t _stat_rx;
static uint32_t _stat_err;

// RF activity LED: each packet sent or received lights it for LED_HOLD_US,
// so that a packet stream looks steady and a single packet still shows.
// MPM v2 RGB LED: TX orange, RX green, RX with a bad CRC red, idle dim blue
// (as at boot). LaunchPad: TX red, RX green, bad CRC both.
#define LED_HOLD_US 20000

typedef enum { LED_IDLE, LED_TX, LED_RX, LED_RX_BAD } led_state_t;

static led_state_t _led_state;
static uint32_t _led_until;

static void _led_show(led_state_t state)
{
#if defined(LED_SPI) && defined(LED_DIN)
  static const uint32_t colors[] = {
      [LED_IDLE] = LED_COLOR(0x00, 0x00, 0x60),
      [LED_TX] = LED_COLOR(0x60, 0x20, 0x00),
      [LED_RX] = LED_COLOR(0x00, 0x60, 0x00),
      [LED_RX_BAD] = LED_COLOR(0x60, 0x00, 0x00),
  };
  led_rgb_set_color(colors[state]);
#elif defined(LED_GREEN) && defined(LED_RED)
  GPIO_writeDio(LED_RED, state == LED_TX || state == LED_RX_BAD);
  GPIO_writeDio(LED_GREEN, state == LED_RX || state == LED_RX_BAD);
#else
  (void)state;
#endif
}

static void _led_event(led_state_t state)
{
  _led_until = micros() + LED_HOLD_US;
  if (state == _led_state) return;
  _led_state = state;
  _led_show(state);
}

static void _led_poll()
{
  if (_led_state == LED_IDLE || micros_before(_led_until)) return;
  _led_state = LED_IDLE;
  _led_show(LED_IDLE);
}

// Acknowledgments (ESB): same framing as the data, empty or with a payload
// (dynamic payload length only). As the nRF24L01+, any valid ack is accepted:
// its packet id is not checked (it differs on acks of retransmissions).
static bool _ack_on;
static uint8_t _ack_retries = 3;
static uint32_t _ack_window = 500;
static uint8_t _ack_max_len = FRAME_NRF_PAYLOAD_MAX;
static frame_t _ack_frame;
static uint32_t _stat_acked;
static uint32_t _stat_retries;

static const char* const _frame_names[] = {"raw",  "nrf",    "xn297",  "cc2500", "a7105",
                                           "cyrf", "hs6200", "rf2500", "frskyl"};

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
  if (_sx) {
    sx1281_set_sync_word(frame_tx_sync(&_frame), frame_rx_sync(&_frame));
    return;
  }
  radio_set_sync_word(frame_tx_sync(&_frame), frame_rx_sync(&_frame));
  radio_set_rx_sync_alt(frame_rx_sync_alt(&_frame));
}

static radio_status_t _radio_init()
{
  if (_sx) {
    radio_off();
    return sx1281_init(&_phy, _sx_dbm);
  }
  sx1281_off();
  return radio_init(&_phy);
}

static radio_status_t _radio_set_frequency(uint32_t khz)
{
  return _sx ? sx1281_set_frequency(khz) : radio_set_frequency(khz);
}

static radio_status_t _radio_send(const uint8_t* data, uint16_t len)
{
  return _sx ? sx1281_send(data, len) : radio_send(data, len);
}

static radio_status_t _radio_rx_start(uint16_t len)
{
  return _sx ? sx1281_rx_start(len) : radio_rx_start(len);
}

static void _radio_rx_stop()
{
  if (_sx) sx1281_rx_stop();
  else radio_rx_stop();
}

static bool _radio_receive(radio_packet_t* pkt)
{
  return _sx ? sx1281_receive(pkt) : radio_receive(pkt);
}

static bool _apply_phy()
{
  _phy.sync_bits = frame_sync_bits(&_frame);
  _phy.tx_power = _tx_power;
  // Measured at 1 Mbps: 0x32 instead of 0x1F gets address 6565656565 from 50%
  // to 100%. Its sensitivity cost is not measured: only used when needed.
  _phy.sync_threshold = _sync_threshold ? _sync_threshold
                        : frame_rx_sync_weak(&_frame) ? SYNC_THRESHOLD_STRICT : 0;
  _phy_ok = _radio_init() == RADIO_OK &&
            _radio_set_frequency(_freq_khz + _trim_khz) == RADIO_OK;
  _set_sync_word();
  return _phy_ok;
}

static void _report_radio_error(const char* what)
{
  _printf("@err %s status=0x%04x\r\n", what, _sx ? sx1281_last_status() : radio_last_status());
}

static void _job_stop()
{
  if (_job == JOB_RX) _radio_rx_stop();
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
static radio_status_t _send_acked(const uint8_t* air, uint16_t n, ack_t* ack)
{
  uint8_t max_len = _ack_frame.dpl ? _ack_max_len : 0;
  uint16_t rx_len = frame_rx_len(&_ack_frame, max_len);
  for (ack->tries = 1; ack->tries <= 1 + _ack_retries; ack->tries++) {
    uint32_t sync = frame_rx_sync(&_ack_frame);
    radio_status_t st = _sx ? sx1281_send_rx(air, n, sync, rx_len, _ack_window, &ack->pkt)
                            : radio_send_rx(air, n, sync, rx_len, _ack_window, &ack->pkt);
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
  uint16_t n = frame_tx_data(&_frame, payload, len, air);
  radio_status_t st = RADIO_ERR_PARAM;
  if (n) st = _ack_on ? _send_acked(air, n, ack) : _radio_send(air, n);
  if (st != RADIO_OK) {
    _stat_err++;
    return false;
  }
  _stat_tx++;
  _led_event(LED_TX);
  if (_ack_on) {
    _stat_retries += ack->tries - 1;
    if (ack->acked) _stat_acked++;
  }
  return true;
}

// Returns true if the frame is valid (CRC)
static bool _report_packet(const radio_packet_t* pkt)
{
  uint8_t payload[FRAME_PAYLOAD_MAX];
  uint8_t len;
  uint16_t pcf;
  frame_result_t res =
      frame_decode_pcf(&_frame, pkt->data, pkt->len, _rx_len, payload, &len, &pcf);

  _led_event(_frame.crc_len && res != FRAME_OK ? LED_RX_BAD : LED_RX);
  _printf("@rx %lu %d ", pkt->timestamp_us, _channel);
  _print_hex(payload, len);
  if (_frame.crc_len) _printf(" crc=%s", res == FRAME_OK ? "ok" : "bad");
  _printf(" rssi=%d", pkt->rssi);
  if (pkt->freq_offset != RADIO_FREQ_OFFSET_NONE) _printf(" foff=%d", pkt->freq_offset);
  if ((_frame.type == FRAME_NRF && _frame.esb) || _frame.type == FRAME_HS6200)
    _printf(" pid=%u", (pcf >> 1) & 3);
  _print("\r\n");
  return res == FRAME_OK;
}

static void _report_ack(const ack_t* ack)
{
  _printf("@ack %lu %d ", ack->pkt.timestamp_us, _channel);
  _print_hex(ack->payload, ack->len);
  _printf(" tries=%u rssi=%d\r\n", ack->tries, ack->pkt.rssi);
}

static uint16_t _rx_var_len(const uint8_t* data, uint16_t n, void* ctx)
{
  (void)ctx;
  return frame_rx_var_len(&_frame, _rx_len, data, n);
}

static bool _rx_start()
{
  uint8_t hdr_len = frame_rx_hdr_len(&_frame);
  uint16_t max_len = frame_rx_len(&_frame, _rx_len);
  if (!_sx && !_rx_fixed && hdr_len)
    return radio_rx_start_var(hdr_len, max_len, _rx_var_len, 0) == RADIO_OK;
  return _radio_rx_start(max_len) == RADIO_OK;
}

//
// Timing: RF core latencies, from its PA and LNA enable outputs (RFC_GPO1,
// RFC_GPO0). Their edges are timestamped in the GPIO edge interrupt
// (get_ticks(), 48 MHz): the CPU may be waiting in a radio call then
//

#if defined(PA_CTX)
  #define TIMING_PA PA_CTX  // the PA/LNA's CTX (board.c), no LNA enable pin
#elif defined(LED_RED) && defined(LED_GREEN)
  #define TIMING_PA LED_RED
  #define TIMING_LNA LED_GREEN
#endif
#define TIMING_EDGES 16
#define TIMING_NONE 0  // no edge found

typedef struct {
  uint32_t ticks;
  uint8_t pin;
  uint8_t level;
} edge_t;

static volatile edge_t _edges[TIMING_EDGES];
static volatile uint8_t _n_edges;

#if defined(TIMING_PA)
static void _edge_pin(uint32_t pin, uint32_t ticks)
{
  if (!GPIO_getEventDio(pin)) return;
  GPIO_clearEventDio(pin);
  if (_n_edges < TIMING_EDGES) _edges[_n_edges++] = (edge_t){ticks, pin, GPIO_readDio(pin)};
}

static void _edge_isr()
{
  uint32_t t = get_ticks();
  _edge_pin(TIMING_PA, t);
  #if defined(TIMING_LNA)
  _edge_pin(TIMING_LNA, t);
  #endif
}
#endif

// Edge capture on (the RF core outputs, input buffer and edge interrupt on)
// or off (the pins as before)
static bool _timing_pins(bool on)
{
#if defined(TIMING_PA)
  static bool isr;
  if (!isr) {
    isr = true;
    IntRegister(INT_AON_GPIO_EDGE, _edge_isr);
  }
  uint32_t capture = IOC_STD_OUTPUT | IOC_INPUT_ENABLE | IOC_BOTH_EDGES | IOC_INT_ENABLE;
  if (on) {
    IOCPortConfigureSet(TIMING_PA, IOC_PORT_RFC_GPO1, capture);
  #if defined(TIMING_LNA)
    IOCPortConfigureSet(TIMING_LNA, IOC_PORT_RFC_GPO0, capture);
  #endif
    GPIO_clearEventDio(TIMING_PA);
    IntPendClear(INT_AON_GPIO_EDGE);
    IntEnable(INT_AON_GPIO_EDGE);
    return true;
  }
  IntDisable(INT_AON_GPIO_EDGE);
  #if defined(PA_CTX)
  IOCPortConfigureSet(PA_CTX, IOC_PORT_RFC_GPO1, IOC_STD_OUTPUT);
  #else
  IOCPinTypeGpioOutput(LED_RED);
  IOCPinTypeGpioOutput(LED_GREEN);
  _led_show(_led_state);
  #endif
  return true;
#else
  (void)on;
  return false;
#endif
}

static void _edges_clear() { _n_edges = 0; }

// First edge of pin to level at or after ticks, TIMING_NONE if none
static uint32_t _edge(uint32_t pin, uint8_t level, uint32_t after)
{
  for (uint8_t i = 0; i < _n_edges; i++)
    if (_edges[i].pin == pin && _edges[i].level == level &&
        _timer_diff(_edges[i].ticks, after) >= 0)
      return _edges[i].ticks;
  return TIMING_NONE;
}

typedef struct {
  const char* name;
  uint32_t n, missed;
  int32_t min, max;  // ticks
  int64_t sum;
} tstat_t;

static void _tstat_add(tstat_t* s, uint32_t from, uint32_t to)
{
  if (from == TIMING_NONE || to == TIMING_NONE) {
    s->missed++;
    return;
  }
  int32_t d = _timer_diff(to, from);
  if (!s->n || d < s->min) s->min = d;
  if (!s->n || d > s->max) s->max = d;
  s->sum += d;
  s->n++;
}

// us with one decimal
static void _print_ticks(const char* name, int32_t ticks)
{
  int32_t t10 = ticks * 10 / 48;
  _printf(" %s=%s%ld.%ld", name, t10 < 0 ? "-" : "", labs(t10) / 10, labs(t10) % 10);
}

// The edges since t0 (first iteration of a measurement): pa+124.1 = PA
// enable rising 124.1 us after t0
static void _print_edges(const char* name, uint32_t t0)
{
  _printf("@edges %s", name);
  for (uint8_t i = 0; i < _n_edges; i++) {
    uint32_t t10 = _timer_diff(_edges[i].ticks, t0) * 10 / 48;
    _printf(" %s%c%lu.%lu", _edges[i].pin == TIMING_PA ? "pa" : "lna", _edges[i].level ? '+' : '-',
            t10 / 10, t10 % 10);
  }
  _print("\r\n");
}

static void _tstat_print(const tstat_t* s)
{
  _printf("@timing %s n=%lu", s->name, s->n);
  if (s->n) {
    _print_ticks("min", s->min);
    _print_ticks("avg", (int32_t)(s->sum / (int64_t)s->n));
    _print_ticks("max", s->max);
  }
  if (s->missed) _printf(" missed=%lu", s->missed);
  _print("\r\n");
}

//
// Scheduler: timer_set_alarm() chained on absolute deadlines, as the MPM
// protocol callbacks (each returns the delay to the next)
//

static void _run_rx();

static uint32_t _sched_at;      // ticks
static uint32_t _sched_period;  // ticks
static volatile uint32_t _sched_left;
static tstat_t _sched_late;

static void _sched_alarm()
{
  _tstat_add(&_sched_late, _sched_at, get_ticks());
  if (--_sched_left == 0) return;
  _sched_at += _sched_period;
  timer_set_alarm(_sched_at, _sched_alarm);
}

static void _cmd_sched(char* args)
{
  uint32_t period, n = 100;
  if (!_number(&args, &period) || period < 10 || period > 10000000) {
    _print("@err usage: sched <period_us> [n]\r\n");
    return;
  }
  _number(&args, &n);
  if (n == 0) n = 1;

  _sched_late = (tstat_t){"sched_late"};
  _sched_period = us2ticks(period);
  _sched_left = n;
  _sched_at = get_ticks() + _sched_period;
  timer_set_alarm(_sched_at, _sched_alarm);
  uint32_t timeout = millis() + (uint32_t)((uint64_t)n * period / 1000) + 1000;
  while (_sched_left && millis_before(timeout)) {
    if (_job == JOB_RX) _run_rx();
  }
  timer_cancel_alarm();
  _tstat_print(&_sched_late);
  _printf("@ok sched period=%lu n=%lu left=%lu\r\n", period, n, _sched_left);
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

static void _print_chip()
{
  _printf("chip=%s", _sx ? "sx1281" : "cc2652");
  if (_sx && _phy_ok) {
    const sx1281_mod_t* m = sx1281_mod();
    _printf(" dbm=%d sx_rate=%lu sx_bw=%lu sx_dev=%lu sx_bt=%s", _sx_dbm, m->rate, m->bw, m->dev,
            m->bt ? "0.5" : "off");
  }
}

static void _cmd_info()
{
  _printf("@ok bench=1 dev=cc2652 phy=%s rate=%lu dev_hz=%lu rx_bw=%u fsk=%d ch=%d freq=%lu "
          "chplan=%lu/%lu framing=%s esb=%d ack=%d pwr=0x%04x trim=%ld autotrim=%s hposc=%d ",
          _phy_name, _phy.symbol_rate, _phy.deviation, _phy.rx_bw, _phy.fsk, _channel,
          _freq_khz, _ch_base_hz, _ch_spacing_hz, _frame_names[_frame.type],
          _frame.type == FRAME_NRF && _frame.esb, _ack_on, _tx_power, _trim_khz,
          _autotrim ? "on" : "off", radio_hposc_offset());
  _print_chip();
  _print("\r\n");
}

// Switches the radio, applied at once if a PHY is set up
static void _cmd_chip(char* args)
{
  char* name = _next_arg(&args);
  bool sx = name && !strcmp(name, "sx1281");
  uint32_t dbm = _number_or(&args, SX1281_TX_DBM_DEFAULT);
  if (!name || (!sx && strcmp(name, "cc2652")) || (int32_t)dbm < -18 ||
      (int32_t)dbm > SX1281_TX_DBM_MAX) {
    _printf("@err usage: chip <cc2652|sx1281> [dbm -18..%d]\r\n", SX1281_TX_DBM_MAX);
    return;
  }
  _job_stop();
  _sx = sx;
  _sx_dbm = (int32_t)dbm;
  if (_phy_ok && !_apply_phy()) {
    _report_radio_error("radio init");
    return;
  }
  _print("@ok ");
  _print_chip();
  _print("\r\n");
}

// PHY presets. nRF24L01: 160 kHz deviation at 250k/1M, 320 kHz at 2M. XN297 at
// 250 kbps: as the MULTI-Module CC2500 emulation (127 kHz). CC2500 ones: the
// MULTI-Module register settings (26 MHz crystal), with their channel plan
// (FREQ, CHANSPC) and framing (SYNC_MODE, CRC_EN, LENGTH_CONFIG).
typedef struct {
  const char* name;
  uint32_t rate;
  uint32_t deviation;
  uint8_t rx_bw;
  bool fsk;
  uint32_t ch_base_hz;
  uint32_t ch_spacing_hz;
  uint8_t sync_len;  // CC2500 framing: 2, 4 = sync word sent twice, A7105: ID
                     // length, 0 = keep the framing
  uint8_t cc2500;    // CC2500 / A7105 framing options, CC_*
  uint8_t preamble;  // preamble bytes, 0 = 4
  uint32_t sync;     // CC2500 SYNC1 SYNC0, 0 = 0xD391. A7105: ID
  bool ble_2m;       // RF core Bluetooth LE 2M PHY (radio_phy_t)
} preset_t;

#define CC_CRC 0x01
#define CC_VAR 0x02    // variable length
#define CC_WHITE 0x04  // whitening
#define CC_FEC 0x08
#define CC_MSK 0x10    // MSK: bits inverted
#define CC_CRC2400 0x20  // CRC_EN with CC2400_EN
#define CC_A7105 0x40    // A7105 framing (CC_CRC, CC_FEC)
#define CC_CYRF 0x80     // CYRF6936 framing (CC_CRC, CC_VAR), CC_FEC: SDR without SOP
#define CC_SOP32 CC_WHITE  // CYRF6936: 32 chip SOP code
#define CC_DATA32 CC_MSK   // CYRF6936: 32 chip data codes

#define NRF_CH_BASE 2400000000
#define NRF_CH_SPACING 1000000
#define FRSKY_CH_BASE 2403999848  // FREQ 0x5C7627
#define FRSKY_CH_SPACING 299927   // CHANSPC 0x7A, E=3
#define A7105_CH_BASE 2400000000
#define A7105_CH_SPACING 500000   // PLL_II CHR 0xF
#define CYRF_CH_BASE 2401000000   // measured: CHANNEL n at 2401 + n MHz
#define CYRF_CH_SPACING 1000000

// DSM codes of PN row 0, SOP column 0 (MULTI-Module DSM_set_sop_data_crc())
static const uint8_t _dsm_sop_code[8] = {0x03, 0xBC, 0x6E, 0x8A, 0xEF, 0xBD, 0xFE, 0xF8};
static const uint8_t _dsm_data_code[16] = {0x40, 0xBA, 0x97, 0xD5, 0x86, 0x4F, 0xCC, 0xD1,
                                           0xD7, 0xA1, 0x54, 0xB1, 0x5E, 0x89, 0xAE, 0x86};
// DSM bind packets: data code of PN row 0, column 8 (DSM_initialize_bind_phase())
#define DSM_BIND_CODE (_dsm_data_code + 8)
// Devo bind: SOP code DEVO_j6pro_sopcodes[0], the CYRF6936 default data codes
// (measured: Devo doesn't set them)
static const uint8_t _devo_sop_code[8] = {0x3C, 0x37, 0xCC, 0x91, 0xE2, 0xF8, 0xCC, 0x91};
static const uint8_t _cyrf_default_code[16] = {0x6F, 0xBE, 0x32, 0x01, 0xDB, 0xF1, 0x2B, 0x01,
                                               0xE3, 0x5C, 0xFA, 0x02, 0x97, 0x93, 0xF9, 0x02};

static const preset_t _presets[] = {
    {"nrf1m", 1000000, 160000, 97, false, NRF_CH_BASE, NRF_CH_SPACING},     // 1399 kHz
    {"nrf2m", 2000000, 320000, 100, false, NRF_CH_BASE, NRF_CH_SPACING},    // 2458 kHz
    // 2 Mbps on the Bluetooth LE 2M PHY (500 kHz deviation): the RF core
    // receives it, unlike its proprietary 2 Mbps PHY
    {"nrf2mble", 2000000, 500000, 100, false, NRF_CH_BASE, NRF_CH_SPACING, 0, 0, 0, 0, true},
    {"nrf250k", 250000, 160000, 94, false, NRF_CH_BASE, NRF_CH_SPACING},    // 882 kHz
    {"xn250k", 250000, 127000, 94, false, NRF_CH_BASE, NRF_CH_SPACING},     // 882 kHz
    // FrSky D: 31.04 kbps GFSK, 31.7 kHz deviation, CRC, sync 15/16
    {"frskyd", 31044, 31750, 85, false, FRSKY_CH_BASE, FRSKY_CH_SPACING, 2, CC_CRC | CC_VAR},  // 175 kHz
    // FrSky X (D16 v1): 70.0 kbps GFSK, 57.1 kHz deviation, sync 30/32, no CRC
    {"frskyx", 70023, 57000, 87, false, FRSKY_CH_BASE, FRSKY_CH_SPACING, 4, CC_VAR},  // 265 kHz
    // FrSky X v2: 77.0 kbps, CRC
    {"frskyx2", 76965, 57000, 87, false, FRSKY_CH_BASE, FRSKY_CH_SPACING, 4, CC_CRC | CC_VAR},
    // FrSky X LBT (EU): 100 kbps 2-FSK, 69.8 kHz deviation, 2405 MHz base. X v2
    // LBT: the same with CRC
    {"frskyxeu", 99976, 69750, 88, true, 2405000000, FRSKY_CH_SPACING, 4, CC_VAR},  // 307 kHz
    {"frskyx2eu", 99976, 69750, 88, true, 2405000000, FRSKY_CH_SPACING, 4, CC_CRC | CC_VAR},
    // HoTT: 250 kbps MSK (as 2-FSK, 62.5 kHz), fixed length, CRC, whitening, FEC.
    // ch = CC2500 CHANNR ((hop + 1) * 3)
    {"hott", 249939, 62500, 91, true, 2403012390, 338013, 4,
     CC_CRC | CC_WHITE | CC_FEC | CC_MSK},  // 530 kHz
    // MSK with a partial phase step (DEVIATN M < 7: Hitec, Redpine, Skyartec)
    // as 2-FSK: the CC2500 wants more than the MSK deviation (rate / 4, Hitec:
    // 99.5%, 0.2 x rate: 71%, 0.3 to 0.64 x rate: 100%), the CC2652 receiver
    // less than 0.4 x rate (Skyartec), with a 350 kHz filter: 0.32 x rate.
    // Hitec: 250 kbps MSK (2/8 phase step), 24 byte preamble, length, CRC,
    // whitening. Data packets: sync word from the ID (cc2500 command)
    {"hitec", 249939, 80000, 89, true, 2405599854, FRSKY_CH_SPACING, 4,
     CC_CRC | CC_VAR | CC_WHITE | CC_MSK, 24},  // 350 kHz
    // Redpine fast: 250 kbps MSK (1/8 phase step), length, CRC. Slow: 10 kbps
    // 2-FSK, 38 kHz deviation, other channel spacing
    {"redpine", 249939, 80000, 89, true, 2432999908, 271362, 4, CC_CRC | CC_VAR | CC_MSK},
    {"redpineslow", 9993, 38000, 85, true, 2432999908, 199951, 4, CC_CRC | CC_VAR},
    // Skyartec: 229 kbps MSK (1/8 phase step), length, CRC. Sync word: 7d7d
    // to bind, from the ID for data (cc2500 command)
    {"skyartec", 228516, 75000, 89, true, 2432999908, 199951, 4, CC_CRC | CC_VAR | CC_MSK, 0,
     0x7D7D},
    // FrSky V8: 27 kbps GFSK, 28.6 kHz deviation, 32 bit sync, length, CRC
    {"frskyv", 26978, 28500, 85, false, 2400999786, FRSKY_CH_SPACING, 4, CC_CRC | CC_VAR},
    // Corona V1 / V2 / Flydream V3: 100 kbps 2-FSK, length, CRC
    {"corona", 99976, 47500, 88, true, 2399999634, 399902, 4, CC_CRC | CC_VAR},
    {"coronav2", 99976, 50750, 88, true, 2405000000, 399902, 4, CC_CRC | CC_VAR},
    {"fdv3", 99976, 50750, 88, true, 2399999634, 399902, 4, CC_CRC | CC_VAR},
    // Ares: 50 kbps 2-FSK, 38 kHz deviation, length, CRC
    {"ares", 49988, 38000, 86, true, 2409999969, FRSKY_CH_SPACING, 4, CC_CRC | CC_VAR, 0, 0x5A60},
    // RadioLink: 38 kbps GFSK, 38 kHz deviation, length, CRC, whitening. Sync
    // DumboRC: badc, RC4G: aba5
    {"radiolink", 37987, 38000, 86, false, 2401999939, 399902, 4, CC_CRC | CC_VAR | CC_WHITE, 0,
     0xABCD},
    // IKEA Ansluta: 250 kbps MSK (2/8 phase step), length, CRC, FEC. ch 16
    {"ikea", 249939, 80000, 89, true, 2432999908, 199951, 4, CC_CRC | CC_VAR | CC_FEC | CC_MSK},
    // Futaba SFHSS: 128 kbps 2-FSK, 38 kHz deviation, fixed length, CC2400 CRC
    {"sfhss", 128143, 38000, 86, true, 2399999634, 249939, 4, CC_CRC2400},  // 220 kHz
    // A7105 (DATA_RATE 0x0E: 500 kbps / (n + 1), TX_II 0x2B), ch = PLL_I.
    // Deviation: the A7105 receives best at 250 kHz (186 kHz: 2.4% CRC errors
    // without FEC, 250 kHz: 0.6%, 140 / 300 kHz: worse).
    // FlySky: 500 kbps, CRC. AFHDS2A: CRC, FEC
    {"flysky", 500000, 250000, 97, false, A7105_CH_BASE, A7105_CH_SPACING, 4, CC_A7105 | CC_CRC, 0,
     0x5475C52A},  // 1399 kHz
    {"afhds2a", 500000, 250000, 97, false, A7105_CH_BASE, A7105_CH_SPACING, 4,
     CC_A7105 | CC_CRC | CC_FEC, 0, 0x5475C52A},
    // Hubsan: 100 kbps, no CRC
    {"hubsan", 100000, 250000, 94, false, A7105_CH_BASE, A7105_CH_SPACING, 4, CC_A7105, 0,
     0x55201041},  // 882 kHz
    // Other A7105 protocols (CODE_I CRCS, FECS; DATA_RATE): IDs of A7105_Init()
    // or 5475C52A (bind ID of the FlySky family, a7105 command for others)
    {"bugs", 250000, 250000, 94, false, A7105_CH_BASE, A7105_CH_SPACING, 4, CC_A7105 | CC_CRC, 0,
     0x5475C52A},
    {"height", 250000, 250000, 94, false, A7105_CH_BASE, A7105_CH_SPACING, 4,
     CC_A7105 | CC_CRC | CC_FEC, 0, 0x25A53C45},
    {"pelikan", 250000, 250000, 94, false, A7105_CH_BASE, A7105_CH_SPACING, 4, CC_A7105, 0,
     0x06230623},
    {"kyosho", 500000, 250000, 97, false, A7105_CH_BASE, A7105_CH_SPACING, 4,
     CC_A7105 | CC_CRC | CC_FEC, 0, 0x5475C52A},
    {"kyoshohype", 250000, 250000, 94, false, A7105_CH_BASE, A7105_CH_SPACING, 4,
     CC_A7105 | CC_CRC | CC_FEC, 0, 0x5475C52A},
    {"wfly2", 500000, 250000, 97, false, A7105_CH_BASE, A7105_CH_SPACING, 4, CC_A7105 | CC_CRC, 0,
     0x5475C52A},
    {"joysway", 500000, 250000, 97, false, A7105_CH_BASE, A7105_CH_SPACING, 4, CC_A7105 | CC_CRC,
     0, 0x5475C52A},
    // CYRF6936 DSM data packets: 1 Mchip/s GFSK, SOP code, length, CRC (seed
    // 0: cyrf command for others), PN row 0 codes. ch = CYRF CHANNEL
    {"dsm", 1000000, 250000, 97, false, CYRF_CH_BASE, CYRF_CH_SPACING, 8,
     CC_CYRF | CC_CRC | CC_VAR, 1},  // the CYRF preamble is in the frame
    // DSM bind: SDR (1 bit per 64 chips), no SOP, length or CRC: 16 bytes = 1 KB
    {"dsmbind", 1000000, 250000, 97, false, CYRF_CH_BASE, CYRF_CH_SPACING, 8, CC_CYRF | CC_FEC, 1},
    // Walkera Devo: 32 chip SOP and data codes, length, CRC (seed 0 as in bind)
    {"devo", 1000000, 250000, 97, false, CYRF_CH_BASE, CYRF_CH_SPACING, 8,
     CC_CYRF | CC_CRC | CC_VAR | CC_SOP32 | CC_DATA32, 1},
    // CYRF6936 GFSK mode (E01X, E129, E010R5): 1 Mbps, CHANNEL n at 2400 + n
    // MHz (measured, TX_OFFSET 0). The 0xAA preamble byte is in the frame
    {"gfsk1m", 1000000, 250000, 97, false, NRF_CH_BASE, NRF_CH_SPACING, 0, 0, 1},
    // FrSky L: 125 kbaud 2-FSK, 47.6 kHz deviation, no sync word: the frame is a
    // chip stream (frskyl framing), FrSky channels
    {"frskyl", 124969, 47500, 88, true, FRSKY_CH_BASE, FRSKY_CH_SPACING},
};

static void _set_cc2500_frame(const uint8_t* sync, uint8_t sync_len, uint8_t options)
{
  _frame.type = FRAME_CC2500;
  memcpy(_frame.addr, sync, sync_len);
  _frame.addr_len = sync_len;
  _frame.crc_len = options & CC_CRC ? 2 : 0;
  _frame.var_len = options & CC_VAR;
  _frame.whitening = options & CC_WHITE;
  _frame.fec = options & CC_FEC;
  _frame.invert = options & CC_MSK;
  _frame.cc2400_crc = options & CC_CRC2400;
  if (_frame.cc2400_crc) _frame.crc_len = 2;
  _frame.prefix_len = 0;
  _frame_ok = frame_init(&_frame);
  _ack_on = false;
}

static void _set_a7105_frame(const uint8_t* id, uint8_t id_len, uint8_t options)
{
  _frame.type = FRAME_A7105;
  memcpy(_frame.addr, id, id_len);
  _frame.addr_len = id_len;
  _frame.crc_len = options & CC_CRC ? 2 : 0;
  _frame.fec = options & CC_FEC;
  _frame.var_len = _frame.whitening = _frame.invert = _frame.cc2400_crc = false;
  _frame.prefix_len = 0;
  _frame_ok = frame_init(&_frame);
  _ack_on = false;
}

// sop: NULL for none. data_code: 16 bytes, SDR: 8
static void _set_cyrf_frame(const uint8_t* sop, const uint8_t* data_code, uint16_t seed,
                            uint8_t options, bool invert)
{
  _frame.type = FRAME_CYRF;
  _frame.sop = sop != NULL;
  _frame.sdr = options & CC_FEC;
  _frame.sop32 = options & CC_SOP32;
  _frame.data32 = options & CC_DATA32;
  memset(_frame.sop_code, 0, sizeof(_frame.sop_code));
  if (sop) memcpy(_frame.sop_code, sop, sizeof(_frame.sop_code));
  memset(_frame.data_code, 0, sizeof(_frame.data_code));
  memcpy(_frame.data_code, data_code, _frame.sdr ? 8 : sizeof(_frame.data_code));
  _frame.crc_seed = seed;
  _frame.crc_len = options & CC_CRC ? 2 : 0;
  _frame.var_len = options & CC_VAR;
  _frame.invert = invert;
  _frame.fec = _frame.whitening = _frame.cc2400_crc = false;
  _frame.prefix_len = 0;
  _frame_ok = frame_init(&_frame);
  _ack_on = false;
}

// FrSky L packets start with 0x4E: a known prefix for the RX sync word
static void _set_frskyl_frame()
{
  _frame.type = FRAME_FRSKYL;
  _frame.crc_len = 0;
  _frame.prefix[0] = 0x4E;
  _frame.prefix_len = 1;
  _frame_ok = frame_init(&_frame);
  _ack_on = false;
}

static bool _set_frequency(uint32_t khz);

static uint32_t _ch_khz(uint32_t ch)
{
  return (_ch_base_hz + (uint64_t)ch * _ch_spacing_hz + 500) / 1000;
}

static bool _load_preset(const char* name)
{
  const preset_t* p = NULL;
  for (uint32_t i = 0; i < sizeof(_presets) / sizeof(_presets[0]); i++)
    if (!strcmp(name, _presets[i].name)) p = &_presets[i];
  if (!p) {
    _printf("@err unknown preset %s\r\n", name);
    return false;
  }

  _job_stop();
  _phy = (radio_phy_t){
      .symbol_rate = p->rate,
      .deviation = p->deviation,
      .rx_bw = p->rx_bw,
      .preamble_bytes = p->preamble ? p->preamble : 4,
      .if_freq = 0x8000,  // RF core default
      .fsk = p->fsk,
      .ble_2m = p->ble_2m,
  };
  strncpy(_phy_name, name, sizeof(_phy_name) - 1);
  _ch_base_hz = p->ch_base_hz;
  _ch_spacing_hz = p->ch_spacing_hz;
  if (_channel >= 0) _freq_khz = _ch_khz(_channel);
  if (!strcmp(name, "frskyl")) {
    _set_frskyl_frame();
  } else if (p->cc2500 & CC_CYRF) {
    if (p->cc2500 & CC_FEC) _set_cyrf_frame(NULL, DSM_BIND_CODE, 0, p->cc2500, false);
    else if (p->cc2500 & CC_DATA32)
      _set_cyrf_frame(_devo_sop_code, _cyrf_default_code, 0, p->cc2500, false);
    else _set_cyrf_frame(_dsm_sop_code, _dsm_data_code, 0, p->cc2500, false);
  } else if (p->sync_len && (p->cc2500 & CC_A7105)) {
    const uint8_t id[] = {p->sync >> 24, p->sync >> 16, p->sync >> 8, p->sync};
    _set_a7105_frame(id, p->sync_len, p->cc2500);
  } else if (p->sync_len) {
    uint16_t sw = p->sync ? p->sync : 0xD391;  // CC2500 default
    const uint8_t sync[] = {sw >> 8, sw & 0xFF, sw >> 8, sw & 0xFF};
    _set_cc2500_frame(sync, p->sync_len, p->cc2500);
  }
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
    _print("@err usage: preset <name>:");
    for (uint32_t i = 0; i < sizeof(_presets) / sizeof(_presets[0]); i++)
      _printf(" %s", _presets[i].name);
    _print("\r\n");
    return;
  }
  if (_load_preset(name)) _printf("@ok preset=%s\r\n", name);
}

static void _cmd_phy(char* args)
{
  uint32_t rate, dev, bw;
  if (!_number(&args, &rate) || !_number(&args, &dev) || !_number(&args, &bw)) {
    _print("@err usage: phy <rate> <dev_hz> <rx_bw> [preamble_bytes] [if_freq] [fsk]\r\n");
    return;
  }

  _job_stop();
  _phy.symbol_rate = rate;
  _phy.deviation = dev;
  _phy.rx_bw = bw;
  _phy.preamble_bytes = _number_or(&args, 4);
  _phy.if_freq = _number_or(&args, 0x8000);
  _phy.fsk = _number_or(&args, 0);
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
  _job_stop();
  if (_phy_ok && !_apply_phy()) {
    _report_radio_error("radio init");
    return;
  }
  _printf("@ok syncthr=0x%02x\r\n", _sync_threshold);
}

// Read 32 bit words. RF core: some registers hang the bus (docs/rf-core-notes.md)
static void _cmd_peek(char* args)
{
  uint32_t addr, n = 1;
  if (!_number(&args, &addr) || (addr & 3)) {
    _print("@err usage: peek <addr> [n]\r\n");
    return;
  }
  _number(&args, &n);
  _print("@ok");
  for (uint32_t i = 0; i < n && i < 16; i++) _printf(" %08lx", *(volatile uint32_t*)(addr + 4 * i));
  _print("\r\n");
}

// Write a 32 bit word. RF core: see _cmd_peek(); some modem registers written
// during RX stop it until the next setup
static void _cmd_poke(char* args)
{
  uint32_t addr, val;
  if (!_number(&args, &addr) || (addr & 3) || !_number(&args, &val)) {
    _print("@err usage: poke <addr> <val>\r\n");
    return;
  }
  *(volatile uint32_t*)addr = val;
  _printf("@ok %08lx\r\n", *(volatile uint32_t*)addr);
}

// Extra RF core override words, applied at once and kept until "ovr" alone
static void _cmd_ovr(char* args)
{
  uint32_t words[RADIO_EXTRA_OVERRIDES];
  uint8_t n = 0;
  while (n < RADIO_EXTRA_OVERRIDES && _number(&args, &words[n])) n++;
  radio_set_extra_overrides(words, n);
  _job_stop();
  if (_phy_ok && !_apply_phy()) {
    _report_radio_error("radio init");
    return;
  }
  _printf("@ok ovr=%u\r\n", n);
}

static void _cmd_pwr(char* args)
{
  uint32_t pwr;
  while (*args == ' ' || *args == '\t') args++;
  if (!strncmp(args, "default", 7)) _tx_power = RF_TX_POWER_DEFAULT;
  else if (_number(&args, &pwr)) _tx_power = pwr;
  // At once on the CC2652 (the SX1281: at the next preset or phy)
  if (_phy_ok && !_sx && radio_set_tx_power(_tx_power) != RADIO_OK) {
    _report_radio_error("pwr");
    return;
  }
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

static void _cmd_cc2500(char* args)
{
  char* sync = _next_arg(&args);
  uint8_t buf[4];
  int len = sync ? _hex(sync, buf, sizeof(buf)) : -1;
  uint8_t options = 0;
  uint32_t crc = _number_or(&args, 1);
  if (crc == 1) options |= CC_CRC;
  if (crc == 2) options |= CC_CRC2400;
  if (_number_or(&args, 1)) options |= CC_VAR;
  if (_number_or(&args, 0)) options |= CC_WHITE;
  if (_number_or(&args, 0)) options |= CC_FEC;
  if (_number_or(&args, 0)) options |= CC_MSK;
  if (len != 2 && len != 4) {
    _print("@err usage: cc2500 <sync: 2 bytes, 4 for SYNC_MODE 3> [crc] [var] [white] [fec] "
           "[msk]\r\n");
    return;
  }

  _job_stop();
  _set_cc2500_frame(buf, len, options);
  if (!_frame_ok) {
    _print("@err bad cc2500 framing\r\n");
    return;
  }
  if (_phy_ok && !_apply_phy()) {
    _report_radio_error("radio init");
    return;
  }
  _printf("@ok framing=cc2500 sync=%08lx crc=%d var=%d white=%d fec=%d msk=%d rx_sync=%08lx\r\n",
          frame_tx_sync(&_frame), _frame.cc2400_crc ? 2 : _frame.crc_len != 0, _frame.var_len,
          _frame.whitening,
          _frame.fec, _frame.invert, frame_rx_sync(&_frame));
}

static void _cmd_a7105(char* args)
{
  char* id = _next_arg(&args);
  uint8_t buf[4];
  int len = id ? _hex(id, buf, sizeof(buf)) : -1;
  uint8_t options = 0;
  if (_number_or(&args, 1)) options |= CC_CRC;
  if (_number_or(&args, 0)) options |= CC_FEC;
  if (len != 2 && len != 4) {
    _print("@err usage: a7105 <id: 2 or 4 bytes> [crc] [fec]\r\n");
    return;
  }

  _job_stop();
  _set_a7105_frame(buf, len, options);
  if (!_frame_ok) {
    _print("@err bad a7105 framing\r\n");
    return;
  }
  if (_phy_ok && !_apply_phy()) {
    _report_radio_error("radio init");
    return;
  }
  _printf("@ok framing=a7105 id=%08lx crc=%d fec=%d rx_sync=%08lx\r\n", frame_tx_sync(&_frame),
          _frame.crc_len != 0, _frame.fec, frame_rx_sync(&_frame));
}

static void _cmd_cyrf(char* args)
{
  char* sop = _next_arg(&args);
  char* data = _next_arg(&args);
  uint8_t sop_buf[8], data_buf[16];
  bool no_sop = sop && !strcmp(sop, "0");
  memset(sop_buf, 0, sizeof(sop_buf));
  int sop_len = no_sop ? 8 : sop ? _hex(sop, sop_buf, sizeof(sop_buf)) : -1;
  int data_len = data ? _hex(data, data_buf, sizeof(data_buf)) : -1;
  uint32_t seed = _number_or(&args, 0);
  uint8_t options = sop_len == 4 ? CC_SOP32 : 0;
  if (_number_or(&args, 1)) options |= CC_CRC;
  if (_number_or(&args, 1)) options |= CC_VAR;
  if (_number_or(&args, 0)) options |= CC_FEC;
  uint32_t chips = _number_or(&args, 64);
  if (chips == 32) options |= CC_DATA32;
  bool invert = _number_or(&args, 0);
  if ((sop_len != 8 && sop_len != 4) || data_len != (options & CC_FEC ? 8 : 16) ||
      seed > 0xFFFF || (chips != 32 && chips != 64)) {
    _print("@err usage: cyrf <sop: 8 bytes, 4: 32 chips, 0: none> <data code: 16 bytes, sdr: 8> "
           "[seed] [crc] [len] [sdr] [data_chips] [invert]\r\n");
    return;
  }

  _job_stop();
  _set_cyrf_frame(no_sop ? NULL : sop_buf, data_buf, seed, options, invert);
  if (!_frame_ok) {
    _print("@err bad cyrf framing\r\n");
    return;
  }
  if (_phy_ok && !_apply_phy()) {
    _report_radio_error("radio init");
    return;
  }
  _printf("@ok framing=cyrf sop=%d seed=%04x crc=%d len=%d sdr=%d chips=%d invert=%d "
          "tx_sync=%08lx rx_sync=%08lx\r\n",
          _frame.sop ? (_frame.sop32 ? 32 : 64) : 0, _frame.crc_seed, _frame.crc_len != 0,
          _frame.var_len, _frame.sdr, _frame.data32 ? 32 : 64, _frame.invert,
          frame_tx_sync(&_frame), frame_rx_sync(&_frame));
}

static void _cmd_hs6200(char* args)
{
  char* addr = _next_arg(&args);
  uint8_t buf[FRAME_ADDR_MAX];
  int len = addr ? _hex(addr, buf, sizeof(buf)) : -1;
  uint32_t crc = _number_or(&args, 1);
  uint32_t scramble = _number_or(&args, 1);
  if (len < 3) {
    _print("@err usage: hs6200 <addr: 3..5 bytes> [crc] [scramble]\r\n");
    return;
  }

  _frame.type = FRAME_HS6200;
  memcpy(_frame.addr, buf, len);
  _frame.addr_len = len;
  _frame.crc_len = crc ? 2 : 0;
  _frame.scramble = scramble;
  _frame.noack = true;  // as the MULTI-Module
  _frame.pid = 0;
  _frame.prefix_len = 0;
  if (!_apply_frame()) {
    if (!_frame_ok) _print("@err bad hs6200 framing\r\n");
    return;
  }
  _printf("@ok framing=hs6200 addr_len=%d crc=%d scramble=%d tx_sync=%08lx rx_sync=%08lx\r\n",
          len, _frame.crc_len != 0, _frame.scramble, frame_tx_sync(&_frame),
          frame_rx_sync(&_frame));
}

static void _cmd_rf2500(char* args)
{
  char* addr = _next_arg(&args);
  uint8_t buf[4];
  int len = addr ? _hex(addr, buf, sizeof(buf)) : -1;
  uint32_t scramble = _number_or(&args, 0);
  uint32_t crc = _number_or(&args, 1);
  if (len != 4) {
    _print("@err usage: rf2500 <addr: 4 bytes> [scramble] [crc]\r\n");
    return;
  }

  _frame.type = FRAME_RF2500;
  memcpy(_frame.addr, buf, len);
  _frame.addr_len = len;
  _frame.crc_len = crc ? 2 : 0;
  _frame.scramble = scramble;
  _frame.prefix_len = 0;
  if (!_apply_frame()) {
    if (!_frame_ok) _print("@err bad rf2500 framing\r\n");
    return;
  }
  _printf("@ok framing=rf2500 crc=%d scramble=%d tx_sync=%08lx rx_sync=%08lx\r\n",
          _frame.crc_len != 0, _frame.scramble, frame_tx_sync(&_frame), frame_rx_sync(&_frame));
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
  if (retries > 15 || window < 100 || window > 100000 || max_len > FRAME_NRF_PAYLOAD_MAX) {
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
  if (job == JOB_RX) _radio_rx_stop();

  _freq_khz = khz;
  bool ok = !_phy_ok || _radio_set_frequency(khz + _trim_khz) == RADIO_OK;
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
  if (!_number(&args, &ch) || ch > 255 || _ch_khz(ch) > FREQ_MAX_KHZ) {
    _print("@err usage: ch <n>, up to 2510 MHz\r\n");
    return;
  }
  uint32_t khz = _ch_khz(ch);
  _channel = ch;
  if (_set_frequency(khz)) _printf("@ok ch=%lu freq=%lu\r\n", ch, khz);
}

static void _autotrim_reset()
{
  _autotrim_sum = 0;
  _autotrim_n = 0;
}

// Returns true when the trim changed
static bool _autotrim_packet(const radio_packet_t* pkt)
{
  if (pkt->freq_offset == RADIO_FREQ_OFFSET_NONE) return false;
  _autotrim_sum += pkt->freq_offset + _autotrim_offset;
  if (++_autotrim_n < AUTOTRIM_PACKETS) return false;

  int32_t step = _autotrim_sum / AUTOTRIM_PACKETS;
  _autotrim_reset();
  if (step > -AUTOTRIM_MIN_KHZ && step < AUTOTRIM_MIN_KHZ) return false;
  int32_t trim = _trim_khz + step;
  if (trim > TRIM_MAX_KHZ) trim = TRIM_MAX_KHZ;
  if (trim < -TRIM_MAX_KHZ) trim = -TRIM_MAX_KHZ;
  if (trim == _trim_khz) return false;
  _trim_khz = trim;
  return true;
}

static void _cmd_autotrim(char* args)
{
  uint32_t offset = 0;
  char* p = args;
  while (*p == ' ') p++;
  if (!strncmp(p, "off", 3)) {
    _autotrim = false;
  } else if (!*p || (_number(&args, &offset) && (int32_t)offset >= -TRIM_MAX_KHZ &&
                     (int32_t)offset <= TRIM_MAX_KHZ)) {
    _autotrim = true;
    _autotrim_offset = (int32_t)offset;
  } else {
    _print("@err usage: autotrim [off | <offset_khz>]\r\n");
    return;
  }
  _autotrim_reset();
  if (_autotrim) _printf("@ok autotrim=%ld trim=%ld\r\n", _autotrim_offset, _trim_khz);
  else _printf("@ok autotrim=off trim=%ld\r\n", _trim_khz);
}

static void _cmd_trim(char* args)
{
  uint32_t trim;
  if (_number(&args, &trim) && (int32_t)trim >= -TRIM_MAX_KHZ && (int32_t)trim <= TRIM_MAX_KHZ) {
    _trim_khz = (int32_t)trim;
    _autotrim_reset();
    if (!_set_frequency(_freq_khz)) return;
  }
  _printf("@ok trim=%ld\r\n", _trim_khz);
}

static void _cmd_chplan(char* args)
{
  uint32_t base, spacing;
  if (!_number(&args, &base) || !_number(&args, &spacing) || base < 2360000000 ||
      base > 2500000000 || spacing > 10000000) {
    _print("@err usage: chplan <base_hz> <spacing_hz>\r\n");
    return;
  }
  _ch_base_hz = base;
  _ch_spacing_hz = spacing;
  _printf("@ok chplan base=%lu spacing=%lu\r\n", base, spacing);
}

static void _cmd_freq(char* args)
{
  uint32_t khz;
  if (!_number(&args, &khz) || khz < 2360000 || khz > FREQ_MAX_KHZ) {
    _print("@err usage: freq <2360000..2510000 kHz>\r\n");
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
  _tx_late_max = 0;
  _seq = 0;
  _stat_tx = _stat_rx = _stat_err = _stat_acked = _stat_retries = 0;
  sx1281_clear_stats();
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
    _print("@err usage: rx <payload_len> [fixed]\r\n");
    return;
  }
  char* opt = _next_arg(&args);
  bool fixed = opt && !strcmp(opt, "fixed");
  if (!_need_phy()) return;
  uint16_t air_len = frame_rx_len(&_frame, len);
  if (air_len == 0 || air_len > RADIO_MAX_PKT_LEN) {
    _print("@err bad rx length for this framing (prefix?)\r\n");
    return;
  }

  _job_stop();
  _rx_len = len;
  _rx_fixed = fixed;
  _stat_tx = _stat_rx = _stat_err = 0;
  if (!_rx_start()) {
    _report_radio_error("rx");
    return;
  }
  _job = JOB_RX;
  _printf("@ok rx len=%lu sync=%08lx", len, frame_rx_sync(&_frame));
  if (frame_rx_sync_alt(&_frame)) _printf(",%08lx", frame_rx_sync_alt(&_frame));
  _printf(" syncthr=0x%02x", _phy.sync_threshold);
  if (!_sx && !fixed && frame_rx_hdr_len(&_frame)) _printf(" var=%u", frame_rx_hdr_len(&_frame));
  _print("\r\n");
}

// Time to settle after a radio call before reading the edges, us
#define TIMING_SETTLE_US 300

static void _cmd_timing(char* args)
{
  uint32_t n = _number_or(&args, 20);
  uint32_t len = _number_or(&args, 16);
  if (n == 0 || n > 1000 || len == 0 || len > FRAME_PAYLOAD_MAX) {
    _print("@err usage: timing [n (1..1000)] [payload_len (1..200)]\r\n");
    return;
  }
  if (_sx) {
    _print("@err timing: cc2652 only\r\n");
    return;
  }
  if (!_need_phy()) return;
  _job_stop();
  if (!_timing_pins(true)) {
    _print("@err timing: no PA enable pin on this board\r\n");
    return;
  }

  // A packet of the current framing (its length byte first if it has one)
  uint8_t payload[FRAME_PAYLOAD_MAX];
  for (uint32_t i = 0; i < len; i++) payload[i] = 0x5A ^ i;
  payload[0] = len - 1;
  uint8_t buf[RADIO_MAX_PKT_LEN];
  uint16_t air_len = frame_tx_data(&_frame, payload, len, buf);
  uint16_t rx_len = frame_rx_len(&_frame, len);
  if (!air_len) {
    _timing_pins(false);
    _print("@err timing: payload length not valid for the framing\r\n");
    return;
  }
  uint32_t f0 = _freq_khz + _trim_khz;
  uint32_t f1 = f0 + 20000 <= FREQ_MAX_KHZ ? f0 + 20000 : f0 - 20000;
  uint32_t sync = frame_rx_sync(&_frame);
  radio_packet_t pkt;
  uint32_t t0, t1, t2, on, off;

  uint32_t air = (_phy.preamble_bytes * 8 + _phy.sync_bits + air_len * 8) * 1000000ULL /
                 _phy.symbol_rate;
  _printf("@timing airtime_us=%lu air_bytes=%u payload=%lu\r\n", air, air_len, len);

  // PHY setup: power up, patch, setup command, synthesizer
  tstat_t init = {"init"};
  for (uint32_t i = 0; i < n && i < 10; i++) {
    t0 = get_ticks();
    radio_init(&_phy);
    _tstat_add(&init, t0, get_ticks());
  }
  radio_set_frequency(f0);

  // Channel change (CMD_FS) while idle: 20 MHz hops, the same frequency
  tstat_t fs = {"fs"}, fs_same = {"fs_same"};
  for (uint32_t i = 0; i < n; i++) {
    t0 = get_ticks();
    radio_set_frequency(i & 1 ? f0 : f1);
    _tstat_add(&fs, t0, get_ticks());
  }
  for (uint32_t i = 0; i < n; i++) {
    t0 = get_ticks();
    radio_set_frequency(f0);
    _tstat_add(&fs_same, t0, get_ticks());
  }

  // TX: call, start of the PA enable, PA on, end seen by radio_tx_wait()
  tstat_t tx_call = {"tx_call"}, tx_start = {"tx_start"}, tx_pa = {"tx_pa"},
          tx_end = {"tx_end"};
  for (uint32_t i = 0; i < n; i++) {
    _edges_clear();
    t0 = get_ticks();
    radio_tx_start(buf, air_len);
    t1 = get_ticks();
    radio_tx_wait();
    t2 = get_ticks();
    delay_us(TIMING_SETTLE_US);
    on = _edge(TIMING_PA, 1, t0);
    off = on == TIMING_NONE ? TIMING_NONE : _edge(TIMING_PA, 0, on);
    _tstat_add(&tx_call, t0, t1);
    _tstat_add(&tx_start, t0, on);
    _tstat_add(&tx_pa, on, off);
    _tstat_add(&tx_end, off, t2);
    if (!i) _print_edges("tx", t0);
  }

  // Hop then TX: radio_set_frequency() + radio_tx_start() to the PA enable
  tstat_t hop_tx = {"hop_tx"};
  for (uint32_t i = 0; i < n; i++) {
    _edges_clear();
    t0 = get_ticks();
    radio_set_frequency(i & 1 ? f0 : f1);
    radio_tx_start(buf, air_len);
    radio_tx_wait();
    delay_us(TIMING_SETTLE_US);
    _tstat_add(&hop_tx, t0, _edge(TIMING_PA, 1, t0));
  }
  radio_set_frequency(f0);

  // RX running, then TX: radio_tx_start() (stops RX) to the PA enable
  tstat_t rx_tx = {"rx_tx"};
  for (uint32_t i = 0; i < n; i++) {
    radio_rx_start(rx_len);
    delay_us(TIMING_SETTLE_US);
    _edges_clear();
    t0 = get_ticks();
    radio_tx_start(buf, air_len);
    radio_tx_wait();
    delay_us(TIMING_SETTLE_US);
    _tstat_add(&rx_tx, t0, _edge(TIMING_PA, 1, t0));
  }

  // TX at an absolute time (radio_tx_start_at(), 1 ms ahead): the PA enable
  // from that time
  tstat_t tx_at = {"tx_at"};
  for (uint32_t i = 0; i < n; i++) {
    _edges_clear();
    bool masked = IntMasterDisable();
    t0 = get_ticks();
    uint32_t at = micros() + 1000;
    if (!masked) IntMasterEnable();
    radio_tx_start_at(buf, air_len, at);
    radio_tx_wait();
    delay_us(TIMING_SETTLE_US);
    _tstat_add(&tx_at, t0 + us2ticks(1000), _edge(TIMING_PA, 1, t0));
  }

  // Instant RSSI: radio_rx_start() to the first valid radio_rssi(), one call
  tstat_t rssi_valid = {"rssi_valid"}, rssi_call = {"rssi_call"};
  int rssi_min = 127, rssi_max = -128;
  for (uint32_t i = 0; i < n; i++) {
    t0 = get_ticks();
    radio_rx_start(rx_len);
    int8_t rssi;
    do {
      t1 = get_ticks();
      rssi = radio_rssi();
      t2 = get_ticks();
    } while (rssi == RADIO_RSSI_NONE && _timer_diff(t2, t0) < (int32_t)us2ticks(5000));
    _tstat_add(&rssi_valid, t0, rssi == RADIO_RSSI_NONE ? TIMING_NONE : t2);
    _tstat_add(&rssi_call, t1, t2);
    if (rssi != RADIO_RSSI_NONE && rssi < rssi_min) rssi_min = rssi;
    if (rssi != RADIO_RSSI_NONE && rssi > rssi_max) rssi_max = rssi;
    radio_rx_stop();
  }

  // RX start and stop calls; with the LNA enable: when RX is on and off
  tstat_t rx_call = {"rx_call"}, rx_on = {"rx_on"}, rx_stop = {"rx_stop"},
          rx_off = {"rx_off"};
  tstat_t hop_rx = {"hop_rx"}, tx_rx = {"tx_rx"}, chain_tx_rx = {"chain_tx_rx"},
          chain_window = {"chain_window"};
  for (uint32_t i = 0; i < n; i++) {
    _edges_clear();
    t0 = get_ticks();
    radio_rx_start(rx_len);
    t1 = get_ticks();
    delay_us(TIMING_SETTLE_US);
    t2 = get_ticks();
    radio_rx_stop();
    uint32_t t3 = get_ticks();
    delay_us(TIMING_SETTLE_US);
    _tstat_add(&rx_call, t0, t1);
    _tstat_add(&rx_stop, t2, t3);
#if defined(TIMING_LNA)
    _tstat_add(&rx_on, t0, _edge(TIMING_LNA, 1, t0));
    _tstat_add(&rx_off, t2, _edge(TIMING_LNA, 0, t2));
    if (!i) _print_edges("rx", t0);
#endif
  }
#if defined(TIMING_LNA)
  // RX hop: RX running, radio_set_frequency() + radio_rx_start() to RX on
  radio_rx_start(rx_len);
  for (uint32_t i = 0; i < n; i++) {
    delay_us(TIMING_SETTLE_US);
    _edges_clear();
    t0 = get_ticks();
    radio_set_frequency(i & 1 ? f0 : f1);
    radio_rx_start(rx_len);
    delay_us(TIMING_SETTLE_US);
    _tstat_add(&hop_rx, t0, _edge(TIMING_LNA, 1, t0));
  }
  radio_rx_stop();
  radio_set_frequency(f0);

  // TX then RX, as a protocol would: radio_tx_wait() + radio_rx_start(), from
  // the end of the PA enable to RX on
  for (uint32_t i = 0; i < n; i++) {
    _edges_clear();
    t0 = get_ticks();
    radio_tx_start(buf, air_len);
    radio_tx_wait();
    radio_rx_start(rx_len);
    delay_us(TIMING_SETTLE_US);
    radio_rx_stop();
    off = _edge(TIMING_PA, 0, t0);
    _tstat_add(&tx_rx, off, off == TIMING_NONE ? TIMING_NONE : _edge(TIMING_LNA, 1, off));
    if (!i) _print_edges("tx_rx", t0);
  }

  // Chained TX -> RX (radio_send_rx(), ESB acks), 200 us window, no answer
  for (uint32_t i = 0; i < n && air_len <= 255 && rx_len <= 255; i++) {
    _edges_clear();
    t0 = get_ticks();
    radio_send_rx(buf, air_len, sync, rx_len, 200, &pkt);
    delay_us(TIMING_SETTLE_US);
    off = _edge(TIMING_PA, 0, t0);
    on = off == TIMING_NONE ? TIMING_NONE : _edge(TIMING_LNA, 1, off);
    _tstat_add(&chain_tx_rx, off, on);
    _tstat_add(&chain_window, on, on == TIMING_NONE ? TIMING_NONE : _edge(TIMING_LNA, 0, on));
    if (!i) _print_edges("chain", t0);
  }
#else
  (void)sync;
  (void)pkt;
#endif

  _timing_pins(false);
  const tstat_t* all[] = {&init,    &fs,          &fs_same,      &tx_call,    &tx_start,
                          &tx_pa,   &tx_end,      &hop_tx,       &rx_tx,      &tx_at,
                          &rx_call, &rx_stop,     &rx_on,        &rx_off,     &hop_rx,
                          &tx_rx,   &chain_tx_rx, &chain_window, &rssi_valid, &rssi_call};
  for (uint32_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
    if (all[i]->n || all[i]->missed) _tstat_print(all[i]);
  _printf("@timing rssi_dbm min=%d max=%d\r\n", rssi_min, rssi_max);

  if (!_apply_phy()) {
    _report_radio_error("radio init");
    return;
  }
  _print("@ok timing\r\n");
}

static void _cmd_stop()
{
  bool was_rx = _job == JOB_RX;
  _job_stop();
  _printf("@ok stop tx=%lu rx=%lu err=%lu", _stat_tx, _stat_rx, _stat_err);
  if (_stat_tx) _printf(" late_max=%lu", _tx_late_max);
  if (_ack_on) _printf(" acked=%lu retries=%lu", _stat_acked, _stat_retries);
  if (_sx) {
    sx1281_stats_t st;
    sx1281_rx_stats(&st);
    _printf(" sx_ok=%lu sx_rearm=%lu sx_busy=%lu sx_last=0x%02x/0x%04x", st.ok, st.rearm, st.busy,
            st.last_status, st.last_irq);
    _printf(" sx_txfail=%lu", st.tx_fail);
    if (_ack_on)
      _printf(" sx_ack=%lu/%lu/%lu sx_turn=%lu", st.ack_rx, st.ack_timeout, st.ack_none,
              st.turn_max);
  }
  if (was_rx && !_sx) {
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
  else if (!strcmp(cmd, "chip")) _cmd_chip(args);
  else if (!strcmp(cmd, "preset")) _cmd_preset(args);
  else if (!strcmp(cmd, "phy")) _cmd_phy(args);
  else if (!strcmp(cmd, "pwr")) _cmd_pwr(args);
  else if (!strcmp(cmd, "syncthr")) _cmd_syncthr(args);
  else if (!strcmp(cmd, "ovr")) _cmd_ovr(args);
  else if (!strcmp(cmd, "peek")) _cmd_peek(args);
  else if (!strcmp(cmd, "poke")) _cmd_poke(args);
  else if (!strcmp(cmd, "sync")) _cmd_sync(args);
  else if (!strcmp(cmd, "nrf")) _cmd_nrf(args);
  else if (!strcmp(cmd, "xn")) _cmd_xn(args);
  else if (!strcmp(cmd, "xaddr")) _cmd_xaddr(args);
  else if (!strcmp(cmd, "prefix")) _cmd_prefix(args);
  else if (!strcmp(cmd, "hs6200")) _cmd_hs6200(args);
  else if (!strcmp(cmd, "rf2500")) _cmd_rf2500(args);
  else if (!strcmp(cmd, "esb")) _cmd_esb(args);
  else if (!strcmp(cmd, "ack")) _cmd_ack(args);
  else if (!strcmp(cmd, "ch")) _cmd_ch(args);
  else if (!strcmp(cmd, "chplan")) _cmd_chplan(args);
  else if (!strcmp(cmd, "trim")) _cmd_trim(args);
  else if (!strcmp(cmd, "autotrim")) _cmd_autotrim(args);
  else if (!strcmp(cmd, "cc2500")) _cmd_cc2500(args);
  else if (!strcmp(cmd, "a7105")) _cmd_a7105(args);
  else if (!strcmp(cmd, "cyrf")) _cmd_cyrf(args);
  else if (!strcmp(cmd, "freq")) _cmd_freq(args);
  else if (!strcmp(cmd, "tx")) _cmd_tx(args);
  else if (!strcmp(cmd, "txp")) _cmd_txp(args);
  else if (!strcmp(cmd, "seq")) _cmd_seq(args);
  else if (!strcmp(cmd, "rx")) _cmd_rx(args);
  else if (!strcmp(cmd, "stop")) _cmd_stop();
  else if (!strcmp(cmd, "timing")) _cmd_timing(args);
  else if (!strcmp(cmd, "sched")) _cmd_sched(args);
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
  uint32_t late = micros() - _tx_next;
  if (late > _tx_late_max) _tx_late_max = late;
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
  bool retune = false;
  while (_radio_receive(&pkt)) {
    _stat_rx++;
    if (_report_packet(&pkt) && _autotrim) retune |= _autotrim_packet(&pkt);
  }
  if (retune) {
    _printf("@trim %ld\r\n", _trim_khz);
    _set_frequency(_freq_khz);
    return;
  }

  // RX ends on errors (e.g. queue overflow): restart it
  if (!_sx && !radio_rx_active()) {
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
    _led_poll();

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
