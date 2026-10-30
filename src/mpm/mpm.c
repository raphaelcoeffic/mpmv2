#include "mpm.h"
#include "mpm_port.h"
#include "mpm_settings.h"
#include "debug.h"

#include <limits.h>
#include <string.h>

// LED blink half periods, ms
#define BLINK_BIND 100
#define BLINK_NO_INPUT 500
#define BLINK_BAD_PROTO_ON 50
#define BLINK_BAD_PROTO_OFF 1000

// Channel order in the status: AETR
#define CHANNEL_ORDER 0xE4

#define STATUS_LEN 24
#define STATUS_NAME_LEN 7
#define STATUS_SUB_NAME_LEN 8

mpm_state_t mpm;

static uint32_t _module_id;

// Protocol
static const mpm_protocol_t* _proto;  // NULL: not in the table
static bool _sub_valid;
static uint16_t (*_callback)();       // NULL: no protocol running
static bool _change;                  // protocol to (re)start

// Last frame: protocol change and bind detection
static mpm_frame_t _cur;
static bool _cur_valid;

// Frame from the radio, waiting to be processed
static uint8_t _rx[MPM_FRAME_MAX_LEN];
static uint8_t _rx_len;
static bool _rx_pending;
// PPM frame waiting to be processed: channel widths, us
static uint16_t _ppm[MPM_CHANNELS];
static uint8_t _ppm_n;
static bool _ppm_pending;
static uint32_t _last_input_ms;
// Input of the last frame (log): none, serial frames, PPM
static enum { INPUT_NONE, INPUT_SERIAL, INPUT_PPM } _input;
static uint8_t _input_ppm_n;

// Callback scheduling
static uint32_t _next_us;
static uint32_t _cb_deadline;  // the running callback's
static uint8_t _count;

static uint32_t _blink_ms;
static bool _led;

static bool _send_status;
static uint32_t _status_ms;

// Protocol list: entry last sent
static int16_t _list_option;

static void _set_led(bool on)
{
  _led = on;
  mpm_port_led(on);
}

void mpm_telem_send(uint8_t type, const uint8_t* data, uint8_t len)
{
  uint8_t buf[MPM_TELEM_HDR_LEN + 255];
  uint8_t n = mpm_telem_header(buf, type, len);
  memcpy(buf + n, data, len);
  mpm_port_write(buf, n + len);
}

void mpm_bind_done()
{
  if (mpm.bind_in_progress) debugln("bind done");
  mpm.bind_in_progress = false;
}

void mpm_sub_invalid() { _sub_valid = false; }

bool mpm_sub_valid() { return _sub_valid; }

uint32_t mpm_callback_deadline() { return _cb_deadline; }

void mpm_send_status() { _send_status = true; }

static void _send_status_frame()
{
  uint8_t d[STATUS_LEN] = {0};
  uint8_t flags = MPM_STATUS_SERIAL;
  if (mpm.input) flags |= MPM_STATUS_INPUT;
  if (_callback) {
    if (_proto && _sub_valid) flags |= MPM_STATUS_PROTO_VALID;
    if (mpm.bind_in_progress) flags |= MPM_STATUS_BINDING;
    if (_proto && _proto->ch_map) flags |= MPM_STATUS_CH_MAP;
    if (_proto && _proto->failsafe) flags |= MPM_STATUS_FAILSAFE;
    if (mpm.data_buffer_low) flags |= MPM_STATUS_BUFFER_LOW;
  }
  d[0] = flags;
  d[1] = MPM_VERSION_MAJOR;
  d[2] = MPM_VERSION_MINOR;
  d[3] = MPM_VERSION_REVISION;
  d[4] = MPM_VERSION_PATCH;
  d[5] = CHANNEL_ORDER;

  if (!_proto) {
    // Not in the table: next and previous = the first one
    d[6] = d[7] = mpm_protocols[0].id;
  } else {
    uint8_t i = _proto - mpm_protocols;
    d[6] = mpm_protocols[i + 1 < mpm_protocol_count ? i + 1 : i].id;
    d[7] = mpm_protocols[i > 0 ? i - 1 : i].id;
    strncpy((char*)d + 8, _proto->name, STATUS_NAME_LEN);
    uint8_t option_type = mpm.option_override <= 0x0F ? mpm.option_override : _proto->option_type;
    d[15] = _proto->n_sub | option_type << 4;
    if (_sub_valid && _proto->n_sub) {
      uint8_t len = _proto->sub_names[0];
      memcpy(d + 16, _proto->sub_names + 1 + len * mpm.sub_protocol,
             len < STATUS_SUB_NAME_LEN ? len : STATUS_SUB_NAME_LEN);
    }
  }
  mpm_telem_send(MPM_TELEM_STATUS, d, STATUS_LEN);
}

// Protocol 0: the protocol table for the radio, the entry given by the option
static uint16_t _proto_list_callback()
{
  uint8_t idx = (uint8_t)mpm.option;
  if (idx == _list_option) return 1000;
  _list_option = idx;

  uint8_t d[1 + STATUS_NAME_LEN + 1 + 3 + 8 * STATUS_SUB_NAME_LEN];
  uint8_t n = 0;
  if (idx >= mpm_protocol_count) {
    // 0xFF: number of protocols
    d[n++] = idx == 0xFF ? mpm_protocol_count : 0xFF;
    debugln("protocol list: %s", idx == 0xFF ? "count" : "end");
  } else {
    const mpm_protocol_t* p = &mpm_protocols[idx];
    d[n++] = p->id;
    for (uint8_t i = 0; i < STATUS_NAME_LEN && p->name[i]; i++) d[n++] = p->name[i];
    d[n++] = 0;
    d[n++] = p->option_type << 4 | (p->ch_map ? 0x02 : 0) | (p->failsafe ? 0x01 : 0);
    d[n++] = p->n_sub;
    if (p->n_sub) {
      uint8_t len = p->sub_names[0];
      memcpy(d + n, p->sub_names, 1 + p->n_sub * len);
      n += 1 + p->n_sub * len;
    }
  }
  mpm_telem_send(MPM_TELEM_PROTO_LIST, d, n);
  return 1000;
}

// Protocol layer hook (upstream/telemetry.c): the protocol's telemetry
__attribute__((weak)) void mpm_protocol_telemetry() {}

// As TelemetryUpdate(): the status first, else the protocol's telemetry
static void _update_telem(uint32_t ms)
{
  if (mpm.disable_telem) return;
  if (_send_status || ms - _status_ms > MPM_STATUS_PERIOD_MS) {
    if (mpm.protocol != MPM_PROTO_LIST) _send_status_frame();
    _send_status = false;
    _status_ms = ms;
    return;
  }
  if (_callback && mpm.protocol != MPM_PROTO_LIST) mpm_protocol_telemetry();
}

// Protocol layer hook (upstream/mpm_compat.c): the protocol data of a radio
// frame (mpm.data: S.Port for the receiver, bind options...)
__attribute__((weak)) void mpm_protocol_frame_data() {}

// Protocol layer hook (upstream/mpm_compat.c): what the MPM's protocol_init()
// resets before a protocol starts
__attribute__((weak)) void mpm_protocol_prepare() {}

// Protocol layer hook (upstream/mpm_compat.c): the RF chips' work between
// callbacks, what the chips do by themselves (a packet received)
__attribute__((weak)) void mpm_protocol_idle() {}

// The protocol (re)started, as the MPM's DEBUG_SERIAL lines
static void _log_start()
{
  if (!_proto) {
    debugln("protocol %d: not in this firmware", mpm.protocol);
    return;
  }
  char sub[16] = "";
  const char* names = _proto->sub_names;
  if (names && mpm.sub_protocol < _proto->n_sub) {
    uint8_t n = names[0] < sizeof(sub) - 1 ? names[0] : sizeof(sub) - 1;
    memcpy(sub, names + 1 + mpm.sub_protocol * names[0], n);
    sub[n] = 0;
    while (n && (sub[n - 1] == ' ' || sub[n - 1] == 0)) sub[--n] = 0;
  }
  if (!_sub_valid) {
    debugln("%s: no sub protocol %d", _proto->name, mpm.sub_protocol);
    return;
  }
  debugln("%s (%d) sub %d %s rx_num %d option %d%s%s", _proto->name, mpm.protocol,
          mpm.sub_protocol, sub, mpm.rx_num, mpm.option, mpm.bind_in_progress ? ", bind" : "",
          mpm.low_power ? ", low power" : "");
}

static void _protocol_init()
{
  _callback = 0;
  _set_led(false);
  _proto = 0;
  _sub_valid = false;
  mpm.failsafe_valid = false;
  mpm.option_override = 0xFF;
  mpm.data_buffer_low = false;
  mpm_port_reset_rf();

  mpm.id_master = _module_id;
  mpm.id = _module_id + mpm.rx_num;
  mpm.rx_tx_addr[0] = mpm.id >> 24;
  mpm.rx_tx_addr[1] = mpm.id >> 16;
  mpm.rx_tx_addr[2] = mpm.id >> 8;
  mpm.rx_tx_addr[3] = mpm.id;
  mpm.rx_tx_addr[4] = (mpm.rx_tx_addr[2] & 0xF0) | (mpm.rx_tx_addr[3] & 0x0F);

  uint32_t ms = mpm_port_millis();
  _blink_ms = ms;

  if (mpm.protocol == MPM_PROTO_LIST) {
    debugln("protocol list");
    _callback = _proto_list_callback;
    _list_option = -1;
    _next_us = mpm_port_micros();
  } else {
    for (uint8_t i = 0; i < mpm_protocol_count; i++) {
      const mpm_protocol_t* p = &mpm_protocols[i];
      if (p->id != mpm.protocol) continue;
      _proto = p;
      _sub_valid = mpm.sub_protocol == 0 || mpm.sub_protocol < p->n_sub;
      if (_sub_valid) {
        mpm_protocol_prepare();
        p->init();
      }
      if (_sub_valid) _callback = p->callback;
      break;
    }
    _log_start();
    _send_status = true;
    _update_telem(ms);
    _next_us = mpm_port_micros() + MPM_FIRST_CALLBACK_US;
  }
  mpm.proto_valid = _proto && _sub_valid && _callback;
  _change = false;
}

static void _end_bind()
{
  if (_proto && _proto->end_bind)
    _proto->end_bind();
  else
    mpm_bind_done();
}

static void _process_frame(const mpm_frame_t* f)
{
  mpm.option = f->option;
  mpm.autobind = f->autobind;
  mpm.range_check = f->range_check;
  mpm.low_power = f->low_power;
  mpm.disable_telem = f->disable_telem;
  mpm.telem_invert = f->telem_invert;
  mpm.disable_ch_map = f->disable_ch_map && _proto && _proto->ch_map;
  memcpy(mpm.data, f->data, f->data_len);
  mpm.data_len = f->data_len;

  if (f->failsafe) {
    memcpy(mpm.failsafe, f->channels, sizeof(mpm.failsafe));
    mpm.failsafe_valid = true;
  } else {
    memcpy(mpm.channels, f->channels, sizeof(mpm.channels));
  }

  // A model change restarts the protocol, the bind flag alone too. Range
  // check and power do not
  if (!_cur_valid || f->protocol != _cur.protocol || f->sub_protocol != _cur.sub_protocol ||
      f->rx_num != _cur.rx_num || f->autobind != _cur.autobind) {
    _change = true;
    mpm.bind_in_progress = f->bind || f->autobind;
    mpm.protocol = f->protocol;
    mpm.sub_protocol = f->sub_protocol;
    mpm.rx_num = f->rx_num;
  } else if (f->bind && !_cur.bind) {
    _change = true;
    mpm.bind_in_progress = true;
  } else if (!f->bind && _cur.bind) {
    _end_bind();
  }
  _cur = *f;
  _cur_valid = true;

  mpm.channel_aux = 0;
  for (uint8_t i = 0; i < 8; i++)
    if (mpm.channels[4 + i] > MPM_CHANNEL_SWITCH) mpm.channel_aux |= 1 << i;

  mpm_protocol_frame_data();
}

// A PPM frame as a serial one, as the MPM's PPM mode: the model of the
// settings (mpm_ppm), the channels scaled from the widths
static void _ppm_frame(mpm_frame_t* f)
{
  const mpm_ppm_settings_t* s = &mpm_ppm;
  memset(f, 0, sizeof(*f));
  f->protocol = s->protocol;
  f->sub_protocol = s->sub_protocol;
  f->rx_num = s->rx_num;
  f->option = s->option;
  f->autobind = s->autobind;
  f->low_power = s->low_power;
  f->disable_telem = true;
  for (uint8_t i = 0; i < MPM_CHANNELS; i++) {
    if (i >= _ppm_n) {
      f->channels[i] = MPM_CHANNEL_CENTER;
      continue;
    }
    int32_t v = ((int32_t)_ppm[i] - s->min_us) * (MPM_CHANNEL_MAX - MPM_CHANNEL_MIN) /
                    (s->max_us - s->min_us) + MPM_CHANNEL_MIN;
    f->channels[i] = v < 0 ? 0 : v > 2047 ? 2047 : v;
  }
}

static void _update_led(uint32_t ms)
{
  if (mpm.input && ms - _last_input_ms > MPM_INPUT_TIMEOUT_MS) {
    mpm.input = false;
    _input = INPUT_NONE;
    debugln("input lost");
  }

  if ((int32_t)(ms - _blink_ms) < 0) return;
  if (!mpm.input) {
    _blink_ms += BLINK_NO_INPUT;
  } else if (!_callback) {
    // Invalid protocol: short flashes
    _blink_ms += _led ? BLINK_BAD_PROTO_OFF : BLINK_BAD_PROTO_ON;
  } else {
    // Binding: fast blink, then on
    if (!mpm.bind_in_progress) _led = false;
    _blink_ms += BLINK_BIND;
  }
  if ((int32_t)(ms - _blink_ms) >= 0) _blink_ms = ms;
  _set_led(!_led);
}

// Processes the input: true if the protocol has been (re)started
static bool _update_all()
{
  uint32_t ms = mpm_port_millis();
  mpm_frame_t f;
  if (_rx_pending) {
    _rx_pending = false;
    if (mpm_frame_decode(_rx, _rx_len, &f)) {
      if (_input != INPUT_SERIAL) debugln("input: serial frames");
      _input = INPUT_SERIAL;
      _process_frame(&f);
      mpm.input = true;
      _last_input_ms = ms;
    }
  }
  if (_ppm_pending) {
    _ppm_pending = false;
    if (_input != INPUT_PPM || _ppm_n != _input_ppm_n) debugln("input: PPM, %d channels", _ppm_n);
    _input = INPUT_PPM;
    _input_ppm_n = _ppm_n;
    _ppm_frame(&f);
    _process_frame(&f);
    mpm.input = true;
    _last_input_ms = ms;
  }
  _update_led(ms);
  _update_telem(ms);
  if (!_change) return false;
  _protocol_init();
  return true;
}

void mpm_init()
{
  memset(&mpm, 0, sizeof(mpm));
  for (uint8_t i = 0; i < MPM_CHANNELS; i++) mpm.channels[i] = MPM_CHANNEL_CENTER;
  _proto = 0;
  _callback = 0;
  _change = false;
  _cur_valid = false;
  _rx_pending = false;
  _ppm_pending = false;
  _input = INPUT_NONE;
  _count = 0;
  _send_status = false;
  _module_id = mpm_port_module_id();
  mpm_port_init();
  _blink_ms = _status_ms = mpm_port_millis();
  _set_led(false);
}

static void _poll()
{
  uint8_t len;
  if (mpm_port_read_frame(_rx, &len)) {
    _rx_len = len;
    _rx_pending = true;
  }
  uint8_t n;
  if (mpm_port_read_ppm(_ppm, &n)) {
    _ppm_n = n;
    _ppm_pending = true;
  }

  // Nothing to run: wait for input, the next callback right after
  uint32_t now = mpm_port_micros();
  if (!_callback || !mpm.input) {
    if (!_update_all()) _next_us = mpm_port_micros();
    return;
  }

  // Input between callbacks, if there is time for it
  int32_t left = _next_us - now;
  if (left > 0) {
    if (left > MPM_IDLE_MIN_US) {
      mpm_protocol_idle();
      left = _next_us - mpm_port_micros();
    }
    if (left > MPM_UPDATE_MIN_US) {
      _count = 0;
      _update_all();
    }
    return;
  }

  _cb_deadline = _next_us;
  _next_us += _callback();

  // Late: the next callback right away. Else make sure the input gets
  // processed even if the protocol never leaves enough time for it
  if ((int32_t)(_next_us - mpm_port_micros()) < 0) return;
  if ((_rx_pending || _ppm_pending) && ++_count > MPM_UPDATE_FORCE) {
    _count = 0;
    _update_all();
  }
}

void mpm_poll()
{
  static bool in_port_idle;
  _poll();
  if (in_port_idle) return;
  in_port_idle = true;
  mpm_port_idle();
  in_port_idle = false;
}

int32_t mpm_time_left()
{
  if (!_callback || !mpm.input) return INT32_MAX;
  return _next_us - mpm_port_micros();
}

void mpm_run()
{
  for (;;) mpm_poll();
}
