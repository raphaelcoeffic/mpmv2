// MULTI-Module core on the board: the radio link, 100000 baud 8E2, on UART0
// (LaunchPad: XDS110 back channel, MPM v2: USB console), not inverted, and
// on the MPM v2 also the module bay on UART1: in on the PPM pin (MODULE_RX,
// inverted, as a radio's external bay sends), telemetry out on S.Port
// (MODULE_TX through U9). Telemetry goes back on the link of the last frame.
//
// S.Port stays released (board_init()) until the first frame from the bay,
// then is inverted (TX1_INV set, U9 holds it low at idle) unless the frame's
// telemetry invert bit is clear (EdgeTX: an internal module), as the STM32
// MULTI-Module switches its TX_INV.
//
// The bay's input is serial or PPM, detected on DIO22 as the edge times
// show (ppm.c: 16 intervals sampled after a gap; 4 channel widths in a row:
// PPM, at most one interval above 200 us: serial), then UART1 or the PPM
// capture. No frame for BAY_LOST_MS: detected again. PPM goes to the core
// as channel widths (mpm_port_read_ppm()), S.Port stays released.
//
// MPM v2 without USB: U8 puts S.Port on UART0 RX, which then hears the
// module's own telemetry (not inverted back: UART errors, the frames are
// dropped; read right, "MP" isn't a frame header either)
#include "mpm_port.h"

#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <inc/hw_fcfg1.h>
#include <inc/hw_memmap.h>
#include <inc/hw_types.h>
#include <string.h>

#include "board.h"
#include "eeprom.h"
#include "file_server.h"
#include "debug.h"
#include "log.h"
#include "mpm.h"
#include "mpm_frame.h"
#include "ppm.h"
#include "radio.h"
#include "timer.h"
#include "uart.h"

#if defined(LED_GREEN)
#elif defined(LED_SPI) && defined(LED_DIN)
  #include "led_rgb.h"
  #define LED_ON_COLOR LED_COLOR(0x00, 0x40, 0x00)
#endif

#define MPM_BAUDRATE 100000

typedef struct {
  uart_t uart;
  uint8_t rx_buf[160];  // MULTI frames, file requests (USB)
  uint8_t tx_buf[256];  // power of 2
  volatile bool rx_error;
} link_t;

static link_t _links[] = {
    {.uart = UART0},
#if defined(MODULE_RX)
    {.uart = UART1},
#endif
};

#define LINK_USB 0
#define LINK_BAY 1

static uint8_t _frame[MPM_FRAME_MAX_LEN];
static uint8_t _frame_len;
static uint8_t _frame_link;
static volatile bool _frame_ready;
// Link of the last frame read: telemetry goes there
static uint8_t _link;

// File request from the USB link ("FS" and file_server.h's request), served
// by mpm_port_idle()
#define FILE_REQ_MAGIC "FS"
static uint8_t _file_req[FILE_SERVER_REQ_MAX];
static uint8_t _file_req_len;
static volatile bool _file_req_ready;

uint32_t mpm_port_micros() { return micros(); }
uint32_t mpm_port_millis() { return millis(); }

// RX timeout: the radio sends a frame in one go, at least 3 bytes of silence
// (32 bits) after it. Frames with UART errors are dropped, as the MPM does
static void _frame_received(uint8_t i)
{
  link_t* l = &_links[i];
  uint32_t len = uart_get_rx_len(l->uart);
  if (i == LINK_USB && len >= 2 && memcmp(l->rx_buf, FILE_REQ_MAGIC, 2) == 0) {
    // one at a time: the host waits for the reply
    if (!l->rx_error && !_file_req_ready && len - 2 <= sizeof(_file_req)) {
      memcpy(_file_req, l->rx_buf + 2, len - 2);
      _file_req_len = len - 2;
      _file_req_ready = true;
    }
  } else if (!l->rx_error && len <= MPM_FRAME_MAX_LEN) {
    memcpy(_frame, l->rx_buf, len);
    _frame_len = len;
    _frame_link = i;
    _frame_ready = true;
  }
  l->rx_error = false;
  uart_reset_rx_len(l->uart);
}

static void _frame_received_usb() { _frame_received(LINK_USB); }
static void _uart_error_usb(uart_error_t error)
{
  (void)error;
  _links[LINK_USB].rx_error = true;
}

static void _link_init(uint8_t i, uint32_t rx, uint32_t tx, void (*frame_received)(),
                       void (*error)(uart_error_t))
{
  link_t* l = &_links[i];
  uart_device_t dev = {
      .mode = UART_8E2,
      .baud_rate = MPM_BAUDRATE,
      .rx = rx,
      .tx = tx,
      .cts = IOID_UNUSED,
      .rts = IOID_UNUSED,
  };
  uart_init(l->uart, &dev);
  uart_callbacks_t cb = {
      .frame_received = frame_received,
      .error = error,
  };
  uart_enable_irqs(l->uart, &cb);
  uart_enable_rx_irq(l->uart, l->rx_buf, sizeof(l->rx_buf));
  uart_enable_tx_irq(l->uart, l->tx_buf, sizeof(l->tx_buf));
}

#if defined(MODULE_RX)
// No frame for this long (ms): the input is detected again
#define BAY_LOST_MS 1000
// Sampling not done after this long (ms): the edges stopped, start again
#define BAY_DETECT_MS 200
// PPM frame end: this long after the last channel pulse (us)
#define BAY_PPM_TIMEOUT_US 2400
// Detection: interval above which serial isn't (us)
#define BAY_SERIAL_MAX_US 200

typedef enum {
  BAY_DETECT = 0,
  BAY_SERIAL,
  BAY_PPM,
} bay_mode_t;

static bay_mode_t _bay_mode;
static uint32_t _bay_ms;  // detection start, then the last frame
static int8_t _bay_invert = -1;  // S.Port: -1 released, else TX1_INV

static uint16_t _ppm[PPM_MAX_CHANNELS];
static uint8_t _ppm_n;
static volatile bool _ppm_ready;

static void _frame_received_bay()
{
  if (_bay_mode == BAY_SERIAL) {
    _frame_received(LINK_BAY);
  } else {
    // UART1 RX is on no pin
    _links[LINK_BAY].rx_error = false;
    uart_reset_rx_len(UART1);
  }
}
static void _uart_error_bay(uart_error_t error)
{
  (void)error;
  _links[LINK_BAY].rx_error = true;
}

// The UART's idle level (1) gives a released S.Port with TX1_INV clear, the
// idle level of inverted serial (low) with it set
static void _bay_telem_invert(bool invert)
{
  if (_bay_invert == invert) return;
  GPIO_writeDio(MODULE_TX_INV, invert);
  if (_bay_invert < 0) IOCPortConfigureSet(MODULE_TX, IOC_PORT_MCU_UART1_TX, IOC_STD_OUTPUT);
  _bay_invert = invert;
}

// From the interrupt, BAY_PPM_TIMEOUT_US after the last channel pulse
static void _ppm_frame(const uint16_t* ppm, uint32_t channels)
{
  if (channels < PPM_MIN_CHANNELS) return;
  memcpy(_ppm, ppm, channels * sizeof(_ppm[0]));
  _ppm_n = channels;
  _ppm_ready = true;
}

static void _bay_detect()
{
  _bay_mode = BAY_DETECT;
  _bay_ms = millis();
  ppm_timer_init(MODULE_RX);
  ppm_start_detect();
}

// As analyse_input_signal() was in main.c
static bay_mode_t _bay_analyse()
{
  ppm_stop_detect();
  uint32_t n = ppm_get_valid_channels();
  const uint16_t* t = ppm_get_channels();

  uint32_t channels = 0;
  while (channels < n && t[channels] >= PULSE_MIN_US && t[channels] <= PULSE_MAX_US) channels++;
  if (channels >= PPM_MIN_CHANNELS) return BAY_PPM;

  uint32_t long_pulses = 0;
  for (uint32_t i = 0; i < n; i++)
    if (t[i] > BAY_SERIAL_MAX_US) long_pulses++;
  return long_pulses <= 1 ? BAY_SERIAL : BAY_DETECT;
}

static void _bay_poll()
{
  uint32_t ms = millis();
  if (_bay_mode != BAY_DETECT) {
    if (ms - _bay_ms <= BAY_LOST_MS) return;
    debugln("bay: no input");
    if (_bay_mode == BAY_PPM) ppm_timer_set_timeout(0, 0);
    _bay_detect();
    return;
  }

  if (!ppm_detect_done()) {
    if (ms - _bay_ms > BAY_DETECT_MS) {
      ppm_timer_stop();
      _bay_detect();
    }
    return;
  }

  bay_mode_t mode = _bay_analyse();
  if (mode == BAY_DETECT) {
    _bay_detect();
    return;
  }
  _bay_mode = mode;
  _bay_ms = ms;
  debugln("bay: %s", mode == BAY_PPM ? "PPM" : "serial");
  if (mode == BAY_PPM) {
    _ppm_ready = false;
    ppm_timer_start();
    ppm_timer_set_timeout(BAY_PPM_TIMEOUT_US, _ppm_frame);
  } else {
    // the frame in progress is lost (its start missed)
    bool masked = IntMasterDisable();
    _links[LINK_BAY].rx_error = false;
    uart_reset_rx_len(UART1);
    if (!masked) IntMasterEnable();
    IOCPortConfigureSet(MODULE_RX, IOC_PORT_MCU_UART1_RX, IOC_STD_INPUT | IOC_IOMODE_INV);
  }
}

static void _bay_init()
{
  // TX on a pin with nothing on it until the first frame: MODULE_TX stays
  // the GPIO of board_init() (S.Port released). RX: after the detection
  _link_init(LINK_BAY, IOID_UNUSED, IOID_UNUSED, _frame_received_bay, _uart_error_bay);
  _bay_detect();
}
#endif

void mpm_port_init()
{
  _link_init(LINK_USB, SERIAL_RX_IOD, SERIAL_TX_IOD, _frame_received_usb, _uart_error_usb);
#if defined(MODULE_RX)
  _bay_init();
#endif
}

bool mpm_port_read_frame(uint8_t* buf, uint8_t* len)
{
#if defined(MODULE_RX)
  _bay_poll();
#endif
  if (!_frame_ready) return false;
  bool masked = IntMasterDisable();
  memcpy(buf, _frame, _frame_len);
  *len = _frame_len;
  _link = _frame_link;
  _frame_ready = false;
  if (!masked) IntMasterEnable();
#if defined(MODULE_RX)
  // Only MULTI frames count (PPM read as serial also gives bytes). V1
  // frames (no flags byte): inverted, a radio's external bay
  mpm_frame_t f;
  if (_link == LINK_BAY && mpm_frame_decode(buf, *len, &f)) {
    _bay_ms = millis();
    _bay_telem_invert(*len <= MPM_FRAME_V1_LEN || (buf[MPM_FRAME_V1_LEN] & MPM_FRAME_TELEM_INVERT));
  }
#endif
  return true;
}

bool mpm_port_read_ppm(uint16_t* us, uint8_t* n)
{
#if defined(MODULE_RX)
  if (!_ppm_ready) return false;
  bool masked = IntMasterDisable();
  memcpy(us, _ppm, _ppm_n * sizeof(_ppm[0]));
  *n = _ppm_n;
  _ppm_ready = false;
  if (!masked) IntMasterEnable();
  _bay_ms = millis();
  return true;
#else
  (void)us;
  (void)n;
  return false;
#endif
}

void mpm_port_write(const uint8_t* data, uint8_t len) { uart_tx_irq(_links[_link].uart, data, len); }

uint16_t mpm_port_write_free() { return uart_tx_free(_links[_link].uart); }

void mpm_port_led(bool on)
{
#if defined(LED_GREEN)
  GPIO_writeDio(LED_GREEN, on);
#elif defined(LED_SPI) && defined(LED_DIN)
  led_rgb_set_color(on ? LED_ON_COLOR : 0);
#else
  (void)on;
#endif
}

void mpm_port_reset_rf() { radio_off(); }

// The chip's IEEE 802.15.4 address: its last 32 bits
uint32_t mpm_port_module_id() { return HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_0); }

// A file request: the reply as telemetry on the USB link, when there is
// room for it
static void _file_request()
{
  static uint8_t frame[MPM_TELEM_HDR_LEN + FILE_SERVER_REPLY_MAX];
  uint8_t n = file_server_request(_file_req, _file_req_len, frame + MPM_TELEM_HDR_LEN);
  _file_req_ready = false;
  if (n == 0) return;
  frame[0] = 'M';
  frame[1] = 'P';
  frame[2] = MPM_TELEM_FILE;
  frame[3] = n;
  uart_t uart = _links[LINK_USB].uart;
  while (uart_tx_free(uart) < MPM_TELEM_HDR_LEN + n) mpm_poll();
  uart_tx_irq(uart, frame, MPM_TELEM_HDR_LEN + n);
}

// The log on the USB link in frames of LOG_FRAME bytes, while there is room
// for them: with the radio on USB, its telemetry keeps LOG_RESERVE bytes
#define LOG_FRAME 64
#define LOG_RESERVE 64
static void _log_send()
{
  uart_t uart = _links[LINK_USB].uart;
  uint16_t room = MPM_TELEM_HDR_LEN + LOG_FRAME + (_link == LINK_USB ? LOG_RESERVE : 0);
  while (log_pending() && uart_tx_free(uart) >= room) {
    uint8_t frame[MPM_TELEM_HDR_LEN + LOG_FRAME];
    uint8_t n = log_read((char*)frame + MPM_TELEM_HDR_LEN, LOG_FRAME);
    if (n == 0) break;
    frame[0] = 'M';
    frame[1] = 'P';
    frame[2] = MPM_TELEM_LOG;
    frame[3] = n;
    uart_tx_irq(uart, frame, MPM_TELEM_HDR_LEN + n);
  }
}

// The EEPROM to the flash as soon as a protocol writes it (bind): about
// 55 ms of erase and program, the callbacks run in the flash waits
void mpm_port_idle()
{
  if (eeprom_dirty()) {
    file_server_sync();
    eeprom_flush();
  }
  if (_file_req_ready) _file_request();
  if (log_on) _log_send();
}
