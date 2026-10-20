// MULTI-Module core against a simulated radio: frame format, protocol start,
// callback timing, status and protocol list telemetry, bind, input loss, PPM
// (built with MPM_PPM_PROTOCOL 127, MPM_PPM_SUB_PROTOCOL 2)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpm/mpm.h"
#include "mpm/mpm_port.h"
#include "mpm/mpm_settings.h"
#include "log.h"

static int _fails;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
      _fails++;                                                        \
    }                                                                  \
  } while (0)

//
// Fake port: time advances on each poll, frames from the simulated radio
//

static uint32_t _us;
static uint8_t _frame[MPM_FRAME_MAX_LEN];
static uint8_t _frame_len;
static bool _frame_ready;
static uint8_t _telem[4096];
static uint32_t _telem_len;
static int _resets;

uint32_t mpm_port_micros() { return _us; }
uint32_t mpm_port_millis() { return _us / 1000; }
void mpm_port_init() {}
bool mpm_port_read_frame(uint8_t* buf, uint8_t* len)
{
  if (!_frame_ready) return false;
  _frame_ready = false;
  memcpy(buf, _frame, _frame_len);
  *len = _frame_len;
  return true;
}
static uint16_t _ppm[MPM_CHANNELS];
static uint8_t _ppm_n;
static bool _ppm_ready;
bool mpm_port_read_ppm(uint16_t* us, uint8_t* n)
{
  if (!_ppm_ready) return false;
  _ppm_ready = false;
  memcpy(us, _ppm, sizeof(_ppm));
  *n = _ppm_n;
  return true;
}
uint16_t mpm_port_write_free() { return 256; }

void mpm_port_write(const uint8_t* data, uint8_t len)
{
  if (_telem_len + len <= sizeof(_telem)) memcpy(_telem + _telem_len, data, len);
  _telem_len += len;
}
void mpm_port_led(bool on) { (void)on; }
void mpm_port_reset_rf() { _resets++; }
uint32_t mpm_port_module_id() { return 0x12345678; }

//
// Protocols
//

#define TEST_PERIOD 4000
#define TEST_CB_US 60  // callback duration

static uint32_t _cb_times[4096];
static int _cb_n;
static int _inits;
static int _end_binds;

static void _test_init() { _inits++; }
static uint16_t _test_callback()
{
  if (_cb_n < 4096) _cb_times[_cb_n] = _us;
  _cb_n++;
  _us += TEST_CB_US;
  return TEST_PERIOD;
}
static void _test_end_bind() { _end_binds++; }

static void _nosub_init() { mpm_sub_invalid(); }

const mpm_protocol_t mpm_protocols[] = {
  {10, "Alpha", 0, 0, MPM_OPTION_NONE, false, false, _test_init, _test_callback, 0},
  {127, "Test", "\x04" "One Two Thr ", 3, MPM_OPTION_RFCHAN, true, true, _test_init,
   _test_callback, _test_end_bind},
  {20, "NoSub", "\x03" "A  B  ", 2, MPM_OPTION_OPTION, false, false, _nosub_init,
   _test_callback, 0},
};
const uint8_t mpm_protocol_count = sizeof(mpm_protocols) / sizeof(mpm_protocols[0]);

//
// Simulated radio: one frame every 7 ms
//

#define FRAME_PERIOD 7000

static mpm_frame_t _radio;
static uint32_t _next_frame;
static bool _radio_on = true;

static void _radio_step()
{
  if (_radio_on && (int32_t)(_us - _next_frame) >= 0) {
    _frame_len = mpm_frame_encode(&_radio, _frame);
    _frame_ready = true;
    _next_frame += FRAME_PERIOD;
  }
}

static void _run(uint32_t us)
{
  uint32_t end = _us + us;
  while ((int32_t)(_us - end) < 0) {
    _radio_step();
    mpm_poll();
    _us += 3;
  }
}

//
// Board work: a long job (flash work) in slices, each started only when it
// ends before the next callback (main.c's _flash_yield())
//

#define IDLE_SLICE_US 600   // a flash read and its processing
#define IDLE_SLICE_MIN 1000  // time left for a slice to start

static uint32_t _idle_job_us;
static int _idle_depth, _idle_depth_max;

void mpm_port_idle()
{
  if (++_idle_depth > _idle_depth_max) _idle_depth_max = _idle_depth;
  if (_idle_job_us) {
    uint32_t end = _us + _idle_job_us;
    _idle_job_us = 0;
    while ((int32_t)(_us - end) < 0) {
      _us += IDLE_SLICE_US;
      do {
        _radio_step();
        mpm_poll();
        _us += 3;
      } while (mpm_time_left() < IDLE_SLICE_MIN);
    }
  }
  _idle_depth--;
}

// Telemetry frames received since the last call
typedef struct {
  uint8_t type, len;
  const uint8_t* data;
} telem_t;

static int _telem_frames(telem_t* out, int max)
{
  int n = 0;
  uint32_t i = 0;
  while (i + 4 <= _telem_len && n < max) {
    if (_telem[i] != 'M' || _telem[i + 1] != 'P') {
      printf("bad telemetry header at %u\n", i);
      _fails++;
      break;
    }
    out[n].type = _telem[i + 2];
    out[n].len = _telem[i + 3];
    out[n].data = _telem + i + 4;
    i += 4 + out[n].len;
    n++;
  }
  return n;
}

static void _clear()
{
  _telem_len = 0;
  _cb_n = 0;
}

static const telem_t* _last_status(telem_t* t, int n)
{
  for (int i = n - 1; i >= 0; i--)
    if (t[i].type == MPM_TELEM_STATUS) return &t[i];
  return 0;
}

//
// Frame format against the decoding of the MPM (update_serial_data())
//

static void test_frame()
{
  mpm_frame_t f = {0}, g;
  uint8_t buf[MPM_FRAME_MAX_LEN];
  srand(1);
  for (int k = 0; k < 1000; k++) {
    f.failsafe = rand() & 1;
    f.v2 = rand() & 1;
    f.protocol = rand() & (f.v2 ? 0xFF : 0x3F);
    f.sub_protocol = rand() & 7;
    f.rx_num = rand() & (f.v2 ? 0x3F : 0x0F);
    f.bind = rand() & 1;
    f.autobind = rand() & 1;
    f.range_check = rand() & 1;
    f.low_power = rand() & 1;
    f.option = rand();
    for (int i = 0; i < MPM_CHANNELS; i++) f.channels[i] = rand() & 0x7FF;
    f.telem_invert = f.v2 && (rand() & 1);
    f.disable_telem = f.v2 && (rand() & 1);
    f.disable_ch_map = f.v2 && (rand() & 1);
    f.data_len = f.v2 ? rand() % (MPM_FRAME_DATA_MAX + 1) : 0;
    memset(f.data, 0, sizeof(f.data));
    for (int i = 0; i < f.data_len; i++) f.data[i] = rand();

    uint8_t len = mpm_frame_encode(&f, buf);
    CHECK(len == (f.v2 ? MPM_FRAME_V1_LEN + 1 + f.data_len : MPM_FRAME_V1_LEN));
    CHECK(mpm_frame_decode(buf, len, &g));
    CHECK(memcmp(&f, &g, sizeof(f)) == 0);

    // As the MPM decodes it
    uint8_t rx[40] = {0};
    memcpy(rx, buf, len);
    CHECK((rx[0] & 0xFC) == 0x54);
    CHECK(((rx[0] & 0x02) != 0) == f.failsafe);
    uint8_t protocol = (rx[1] & 0x1F) + (rx[0] & 1 ? 0 : 32);
    if (len > 26) protocol |= rx[26] & 0xC0;
    CHECK(protocol == f.protocol);
    uint8_t rx_num = rx[2] & 0x0F;
    if (len > 26) rx_num |= rx[26] & 0x30;
    CHECK(rx_num == f.rx_num);
    CHECK(((rx[2] >> 4) & 7) == f.sub_protocol);
    const uint8_t* p = rx + 3;
    uint8_t dec = -3;
    for (int i = 0; i < MPM_CHANNELS; i++) {
      dec += 3;
      if (dec >= 8) {
        dec -= 8;
        p++;
      }
      p++;
      uint32_t w = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
      CHECK(((w >> dec) & 0x7FF) == f.channels[i]);
    }
  }

  // Not frames
  CHECK(!mpm_frame_decode(buf, 25, &g));
  CHECK(!mpm_frame_decode(buf, 37, &g));
  buf[0] = 0x50;
  CHECK(!mpm_frame_decode(buf, 26, &g));

  // EdgeTX: channels at 0 % (1024), protocol 127 (V2)
  memset(&f, 0, sizeof(f));
  f.protocol = 127;
  f.v2 = true;
  for (int i = 0; i < MPM_CHANNELS; i++) f.channels[i] = 1024;
  uint8_t len = mpm_frame_encode(&f, buf);
  CHECK(len == 27);
  CHECK(buf[0] == 0x54 && buf[1] == 0x1F && buf[26] == 0x40);
  // 1024 = 0b10000000000: bit 10 set in every 11 bit field
  CHECK(buf[4] == 0x00 && buf[5] == 0x04 && buf[6] == 0x20 && buf[7] == 0x00);
}

//
// Core
//

static void _radio_model(uint8_t protocol, uint8_t sub, uint8_t rx_num)
{
  memset(&_radio, 0, sizeof(_radio));
  _radio.v2 = true;
  _radio.protocol = protocol;
  _radio.sub_protocol = sub;
  _radio.rx_num = rx_num;
  for (int i = 0; i < MPM_CHANNELS; i++) _radio.channels[i] = MPM_CHANNEL_CENTER;
}

static void test_start_and_timing()
{
  _us = 1000000;
  _next_frame = _us + 100;
  _radio_on = true;
  _radio_model(127, 1, 5);
  _radio.option = 7;
  mpm_init();
  _clear();
  _inits = 0;

  _run(2000);
  CHECK(_inits == 1);
  CHECK(mpm.protocol == 127 && mpm.sub_protocol == 1 && mpm.rx_num == 5 && mpm.option == 7);
  CHECK(mpm.proto_valid && mpm.input && !mpm.bind_in_progress);
  CHECK(mpm.id == 0x12345678 + 5);
  CHECK(mpm.rx_tx_addr[0] == 0x12 && mpm.rx_tx_addr[3] == 0x7D && mpm.rx_tx_addr[4] == 0x5D);

  _run(1000000 - 2000);
  // First callback 5 ms after the first frame, then every 4 ms on time
  CHECK(_cb_n > 240);
  int32_t first = _cb_times[0] - 1000100;
  CHECK(first >= 5000 && first < 5010);
  int32_t late_max = 0;
  for (int i = 1; i < _cb_n && i < 4096; i++) {
    int32_t late = _cb_times[i] - (_cb_times[0] + i * TEST_PERIOD);
    if (late < 0) late = -late;
    if (late > late_max) late_max = late;
  }
  printf("callbacks %d, first after %d us, late max %d us\n", _cb_n, first, late_max);
  CHECK(late_max < 10);

  // Status at once, then every 500 ms
  telem_t t[64];
  int n = _telem_frames(t, 64);
  CHECK(n == 2);
  const telem_t* s = _last_status(t, n);
  CHECK(s && s->len == 24);
  if (!s) return;
  const uint8_t* d = s->data;
  CHECK(d[0] == (MPM_STATUS_INPUT | MPM_STATUS_SERIAL | MPM_STATUS_PROTO_VALID |
                 MPM_STATUS_FAILSAFE | MPM_STATUS_CH_MAP));
  CHECK(d[1] == 1 && d[2] == 3 && d[3] == 4 && d[4] == 74);
  CHECK(d[5] == 0xE4);
  CHECK(d[6] == 20 && d[7] == 10);  // next, previous
  CHECK(memcmp(d + 8, "Test\0\0\0", 7) == 0);
  CHECK(d[15] == (3 | MPM_OPTION_RFCHAN << 4));
  CHECK(memcmp(d + 16, "Two \0\0\0\0", 8) == 0);
}

static void test_port_idle()
{
  // A 150 ms job in slices: callbacks on time, the frames processed (no
  // input loss after 70 ms), mpm_port_idle() not nested
  _clear();
  _idle_depth_max = 0;
  _idle_job_us = 150000;
  _run(200000);
  CHECK(_idle_depth_max == 1);
  CHECK(mpm.input);
  CHECK(_cb_n >= 49 && _cb_n <= 51);
  int32_t late_max = 0;
  for (int i = 1; i < _cb_n && i < 4096; i++) {
    int32_t late = _cb_times[i] - (_cb_times[0] + i * TEST_PERIOD);
    if (late < 0) late = -late;
    if (late > late_max) late_max = late;
  }
  printf("port idle: callbacks %d, late max %d us\n", _cb_n, late_max);
  CHECK(late_max < 10);
  CHECK(mpm_time_left() > 0 && mpm_time_left() <= TEST_PERIOD);
}

static void test_input_loss()
{
  // Callbacks stop 70 ms after the last frame, start again with the frames
  _clear();
  _radio_on = false;
  _run(200000);
  CHECK(!mpm.input);
  CHECK(_cb_n >= 17 && _cb_n <= 20);
  _clear();
  _run(100000);
  CHECK(_cb_n == 0);
  _radio_on = true;
  _next_frame = _us;
  _inits = 0;
  _run(100000);
  CHECK(mpm.input && _inits == 0);
  CHECK(_cb_n >= 24 && _cb_n <= 26);
  CHECK(_cb_times[0] - (_next_frame - 15 * FRAME_PERIOD) < 50);
}

static void test_bind()
{
  _inits = _end_binds = 0;
  _clear();
  // Bind flag: restart, binding
  _radio.bind = true;
  _run(20000);
  CHECK(_inits == 1 && mpm.bind_in_progress);
  telem_t t[64];
  int n = _telem_frames(t, 64);
  const telem_t* s = _last_status(t, n);
  CHECK(s && (s->data[0] & MPM_STATUS_BINDING));
  // Flag cleared: the protocol ends bind
  _radio.bind = false;
  _run(20000);
  CHECK(_inits == 1 && _end_binds == 1);
  // Range check: no restart
  _radio.range_check = true;
  _run(20000);
  CHECK(_inits == 1 && mpm.range_check);
  _radio.range_check = false;
  // Autobind is a model change: restart, binding
  _radio.autobind = true;
  _run(20000);
  CHECK(_inits == 2 && mpm.bind_in_progress);
  _radio.autobind = false;
  mpm_bind_done();
  _run(20000);
  CHECK(_inits == 3 && !mpm.bind_in_progress);
  // New rx_num: restart, new id
  _radio.rx_num = 6;
  _run(20000);
  CHECK(_inits == 4 && mpm.id == 0x12345678 + 6);
  // Failsafe frame
  for (int i = 0; i < MPM_CHANNELS; i++) _radio.channels[i] = i == 2 ? 0 : MPM_FAILSAFE_HOLD;
  _radio.failsafe = true;
  _run(10000);
  _radio.failsafe = false;
  for (int i = 0; i < MPM_CHANNELS; i++) _radio.channels[i] = MPM_CHANNEL_MAX;
  _run(10000);
  CHECK(_inits == 4 && mpm.failsafe_valid && mpm.failsafe[2] == 0 && mpm.failsafe[0] == 2047);
  CHECK(mpm.channels[0] == MPM_CHANNEL_MAX && mpm.channel_aux == 0xFF);
}

// New model from the radio: the old protocol runs until its frame arrives
static void _switch(uint8_t protocol, uint8_t sub, uint32_t us)
{
  _radio_model(protocol, sub, 0);
  _run(10000);
  _clear();
  _run(us);
}

static void test_invalid()
{
  // Not in the table
  _switch(99, 0, 600000);
  CHECK(!mpm.proto_valid && _cb_n == 0);
  telem_t t[64];
  int n = _telem_frames(t, 64);
  const telem_t* s = _last_status(t, n);
  CHECK(s && s->data[0] == (MPM_STATUS_INPUT | MPM_STATUS_SERIAL));
  CHECK(s && s->data[6] == 10 && s->data[7] == 10 && s->data[8] == 0);

  // Sub protocol out of range
  _switch(10, 2, 20000);
  CHECK(!mpm.proto_valid && _cb_n == 0);
  // Rejected by the protocol init
  _switch(20, 1, 600000);
  CHECK(!mpm.proto_valid && _cb_n == 0);
  n = _telem_frames(t, 64);
  s = _last_status(t, n);
  CHECK(s && s->data[0] == (MPM_STATUS_INPUT | MPM_STATUS_SERIAL));
  CHECK(s && memcmp(s->data + 8, "NoSub\0\0", 7) == 0 && s->data[16] == 0);
}

static void test_proto_list()
{
  // As EdgeTX scans: 0xFF, then 0, 1, ... until 0xFF comes back
  _radio_model(0, 0, 0);
  _radio.option = -1;
  _clear();
  _run(30000);
  telem_t t[64];
  int n = _telem_frames(t, 64);
  CHECK(n == 1 && t[0].type == MPM_TELEM_PROTO_LIST && t[0].len == 1 && t[0].data[0] == 3);

  static const uint8_t alpha[] = {10, 'A', 'l', 'p', 'h', 'a', 0, 0x00, 0};
  static const uint8_t test[] = {127, 'T', 'e', 's', 't', 0, 0x83, 3, 4,
                                 'O', 'n', 'e', ' ', 'T', 'w', 'o', ' ', 'T', 'h', 'r', ' '};
  for (int i = 0; i < 4; i++) {
    _radio.option = i;
    _clear();
    _run(30000);
    n = _telem_frames(t, 64);
    CHECK(n == 1 && t[0].type == MPM_TELEM_PROTO_LIST);
    if (i == 0) CHECK(t[0].len == sizeof(alpha) && !memcmp(t[0].data, alpha, sizeof(alpha)));
    if (i == 1) CHECK(t[0].len == sizeof(test) && !memcmp(t[0].data, test, sizeof(test)));
    if (i == 3) CHECK(t[0].len == 1 && t[0].data[0] == 0xFF);
  }
}

// PPM: the build's protocol, the widths scaled as the MPM's TX_ER9X, no
// telemetry, input loss as with serial
static void _run_ppm(uint32_t us, uint32_t period)
{
  uint32_t end = _us + us;
  uint32_t next = _us;
  while ((int32_t)(_us - end) < 0) {
    if ((int32_t)(_us - next) >= 0) {
      _ppm_ready = true;
      next += period;
    }
    mpm_poll();
    _us += 3;
  }
}

static void test_ppm()
{
  _us = 3000000;
  _radio_on = false;
  mpm_init();
  _clear();
  _inits = 0;

  static const uint16_t widths[8] = {988, 1500, 2012, 1100, 700, 2400, 1501, 1250};
  static const uint16_t expect[8] = {204, 1024, 1844, 383, 0, 2047, 1025, 623};
  memcpy(_ppm, widths, sizeof(widths));
  _ppm_n = 8;
  _run_ppm(500000, 22500);
  CHECK(_inits == 1);
  CHECK(mpm.protocol == 127 && mpm.sub_protocol == 2 && mpm.rx_num == 0 && mpm.option == 0);
  CHECK(mpm.proto_valid && mpm.input && mpm.disable_telem);
  for (int i = 0; i < 8; i++) CHECK(mpm.channels[i] == expect[i]);
  for (int i = 8; i < MPM_CHANNELS; i++) CHECK(mpm.channels[i] == MPM_CHANNEL_CENTER);
  CHECK(mpm.channel_aux == 0x02);  // channel 6 above the switch level
  CHECK(_telem_len == 0);
  CHECK(_cb_n > 120);
  printf("ppm: callbacks %d\n", _cb_n);

  // Settings changed while running (ppm.txt uploaded): the next frame
  // restarts the protocol with them; the widths for -+100%
  _clear();
  _inits = 0;
  mpm_ppm.protocol = 10;
  mpm_ppm.sub_protocol = 0;
  mpm_ppm.rx_num = 3;
  mpm_ppm.autobind = true;
  mpm_ppm.min_us = 1000;
  mpm_ppm.max_us = 2000;
  static const uint16_t widths2[4] = {1000, 1500, 2000, 1250};
  static const uint16_t expect2[4] = {204, 1024, 1844, 614};
  memcpy(_ppm, widths2, sizeof(widths2));
  _ppm_n = 4;
  _run_ppm(100000, 22500);
  CHECK(_inits == 1);
  CHECK(mpm.protocol == 10 && mpm.sub_protocol == 0 && mpm.rx_num == 3 && mpm.autobind);
  for (int i = 0; i < 4; i++) CHECK(mpm.channels[i] == expect2[i]);
  mpm_ppm_defaults(&mpm_ppm);

  _clear();
  _run(200000);  // no PPM frames
  CHECK(!mpm.input);
}

//
// ppm.txt
//

static void test_ppm_parse()
{
  mpm_ppm_settings_t s;
  char err[512];
  mpm_ppm_defaults(&s);
  CHECK(s.protocol == 127 && s.sub_protocol == 2 && s.min_us == 988 && s.max_us == 2012);

  const char* t1 = "# PPM mode\n"
                   "sub = two\n"
                   "protocol = Test  # the test protocol\n"
                   "rx_num=5\n"
                   "  option = -7\n"
                   "autobind = yes\n"
                   "low_power=on\r\n"
                   "min_us=1000\n"
                   "max_us = 2000";
  CHECK(mpm_ppm_parse(t1, strlen(t1), &s, err, sizeof(err)) == 0 && err[0] == 0);
  CHECK(s.protocol == 127 && s.sub_protocol == 1 && s.rx_num == 5 && s.option == -7);
  CHECK(s.autobind && s.low_power && s.min_us == 1000 && s.max_us == 2000);

  // a new protocol without its sub protocol: the first; names and numbers,
  // case and spaces ignored
  const char* t2 = "protocol = al pha\n";
  CHECK(mpm_ppm_parse(t2, strlen(t2), &s, err, sizeof(err)) == 0);
  CHECK(s.protocol == 10 && s.sub_protocol == 0 && s.rx_num == 5);
  const char* t3 = "protocol = 20\nsub = B\nautobind = 0\n";
  CHECK(mpm_ppm_parse(t3, strlen(t3), &s, err, sizeof(err)) == 0);
  CHECK(s.protocol == 20 && s.sub_protocol == 1 && !s.autobind);
  const char* t4 = "protocol = TEST\nsub = 2";
  CHECK(mpm_ppm_parse(t4, strlen(t4), &s, err, sizeof(err)) == 0 && s.sub_protocol == 2);
  // the option as a byte too
  const char* t4b = "option = 128\n";
  CHECK(mpm_ppm_parse(t4b, strlen(t4b), &s, err, sizeof(err)) == 0 && s.option == -128);

  // bad lines change nothing
  mpm_ppm_settings_t before = s;
  const char* t5 = "protocol = nope\n"
                   "sub = Four\n"
                   "foo = 1\n"
                   "rx_num = 64\n"
                   "min_us = 2100\n"
                   "max_us=2000\n"
                   "junk\n"
                   "option = 12x\n";
  CHECK(mpm_ppm_parse(t5, strlen(t5), &s, err, sizeof(err)) == 7);
  printf("ppm.txt errors:\n%s", err);
  CHECK(strstr(err, "line 1: unknown protocol: nope\n"));
  CHECK(strstr(err, "line 2: no such sub protocol: Four\n"));
  CHECK(strstr(err, "line 3: unknown key: foo\n"));
  CHECK(strstr(err, "line 4: rx_num: 0 to 63: 64\n"));
  CHECK(strstr(err, "line 7: not key = value: junk\n"));
  CHECK(strstr(err, "line 8: option: -128 to 255: 12x\n"));
  CHECK(strstr(err, "min_us must be below max_us\n"));
  CHECK(memcmp(&s, &before, sizeof(s)) == 0);
  // sub protocols out of range, none for a protocol without
  const char* t6 = "protocol = Test\nsub = 3\n";
  CHECK(mpm_ppm_parse(t6, strlen(t6), &s, err, sizeof(err)) == 1);
  const char* t7 = "protocol = Alpha\nsub = 0\n";
  CHECK(mpm_ppm_parse(t7, strlen(t7), &s, err, sizeof(err)) == 0 && s.protocol == 10);
  const char* t8 = "sub = 1\n";
  CHECK(mpm_ppm_parse(t8, strlen(t8), &s, err, sizeof(err)) == 1);

  // messages cut to the buffer, terminated
  char small[20];
  memset(small, 'x', sizeof(small));
  CHECK(mpm_ppm_parse(t5, strlen(t5), &s, small, sizeof(small)) == 7);
  CHECK(strlen(small) == sizeof(small) - 1);
  CHECK(mpm_ppm_parse("", 0, &s, 0, 0) == 0);
}

//
// Debug log (log.c)
//

static uint32_t _log_ms;
static uint32_t _log_millis() { return _log_ms; }

// All the log not read yet, terminated
static const char* _log_text()
{
  static char text[2 * LOG_SIZE];
  uint32_t n = 0, r;
  while ((r = log_read(text + n, 64)) > 0) n += r;
  text[n] = 0;
  return text;
}

static void test_log()
{
  log_init(_log_millis);
  log_enable(LOG_HOST, true);
  _log_text();
  CHECK(log_pending() == 0);

  // the time at each line start, lines in pieces
  _log_ms = 12345;
  log_printf("a %d\n", 5);
  log_printf("b");
  _log_ms = 123456789;
  log_printf("c\nd");
  log_printf("\n");
  CHECK(strcmp(_log_text(), "  12.345 a 5\n  12.345 bc\n123456.789 d\n") == 0);

  // formats
  _log_ms = 1;
  log_printf("[%5d] [%-5d] [%05d] [%x] [%08X] [%02X] [%s] [%6s] [%-6s] [%c] [%u] [%d] [%ld] %%\n",
             -42, 7, -3, 0xbeef, 0x1234abcd, 5, "hi", "ab", "cd", 'z', 4000000000u,
             -2147483647 - 1, 12L);
  const char* t = _log_text();
  printf("log: %s", t);
  CHECK(strcmp(t, "   0.001 [  -42] [7    ] [-0003] [beef] [1234ABCD] [05] [hi] [    ab] [cd    ] "
                  "[z] [4000000000] [-2147483648] [12] %\n") == 0);

  // overwritten before read: their number, then the last LOG_SIZE bytes
  uint32_t written = 0;
  for (int i = 0; i < 500; i++) {
    log_printf("%03d\n", i);  // 9 + 4 bytes
    written += 13;
  }
  CHECK(log_pending() == LOG_SIZE);
  t = _log_text();
  char lost[40];
  snprintf(lost, sizeof(lost), "\n[%u bytes lost]\n", written - LOG_SIZE);
  CHECK(strncmp(t, lost, strlen(lost)) == 0);
  CHECK(strlen(t) == strlen(lost) + LOG_SIZE);
  CHECK(strcmp(t + strlen(t) - 13, "   0.001 499\n") == 0);

  // a host asking again: from the oldest whole line kept
  log_enable(LOG_HOST, false);
  log_enable(LOG_HOST, true);
  t = _log_text();
  CHECK(strncmp(t, "   0.001 ", 9) == 0);
  CHECK(strlen(t) > LOG_SIZE - 13 && strlen(t) <= LOG_SIZE);
  CHECK(strcmp(t + strlen(t) - 13, "   0.001 499\n") == 0);
  log_enable(LOG_HOST, false);
  CHECK(log_on == 0);
}

int main()
{
  test_log();
  test_frame();
  test_start_and_timing();
  test_port_idle();
  test_input_loss();
  test_bind();
  test_invalid();
  test_proto_list();
  test_ppm();
  test_ppm_parse();
  printf("%s\n", _fails ? "FAILED" : "ok");
  return _fails != 0;
}
