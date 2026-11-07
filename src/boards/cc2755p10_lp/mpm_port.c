// MULTI-Module core on the LP-EM-CC2755P10: the link with the radio on
// UART0, 100000 baud 8E2, through the XDS110 back channel (a host plays the
// radio: tools/bench/radio_sim.py), normal polarity. No PPM input, no file
// system: the EEPROM in RAM (eeprom_ram.c), read and written from the host as
// the file "eeprom" (file_server_ram.c: "FS" requests on the link, as on the
// MPM v2's USB), and the debug log sent when a host asks (mpm_fs.py log).
//
// RX in the UART interrupt (platforms/cc27xx/uart.c): a frame ends at the
// receive timeout (32 bit times of silence). Frames with UART errors are
// dropped, as the MPM does. TX from uart.c's ring.
#include "mpm/mpm_port.h"

#include <string.h>

#include <driverlib/interrupt.h>
#include <inc/hw_fcfg.h>

#include "board.h"
#include "eeprom.h"
#include "file_server.h"
#include "log.h"
#include "mpm/mpm.h"
#include "mpm/mpm_frame.h"
#include "radio.h"
#include "timer.h"
#include "uart.h"

#define MPM_BAUDRATE 100000

static uint8_t _rx_buf[160];  // MULTI frames, file requests
static uint8_t _tx_buf[256];  // power of 2
static volatile bool _rx_error;

static uint8_t _frame[MPM_FRAME_MAX_LEN];
static uint8_t _frame_len;
static volatile bool _frame_ready;

// File request ("FS" and file_server.h's request), served by mpm_port_idle()
#define FILE_REQ_MAGIC "FS"
static uint8_t _file_req[FILE_SERVER_REQ_MAX];
static uint8_t _file_req_len;
static volatile bool _file_req_ready;

uint32_t mpm_port_micros() { return micros(); }
uint32_t mpm_port_millis() { return millis(); }

static void _frame_received()
{
  uint32_t len = uart_get_rx_len(UART0);
  if (len >= 2 && memcmp(_rx_buf, FILE_REQ_MAGIC, 2) == 0) {
    // one at a time: the host waits for the reply
    if (!_rx_error && !_file_req_ready && len - 2 <= sizeof(_file_req)) {
      memcpy(_file_req, _rx_buf + 2, len - 2);
      _file_req_len = len - 2;
      _file_req_ready = true;
    }
  } else if (!_rx_error && len <= MPM_FRAME_MAX_LEN) {
    memcpy(_frame, _rx_buf, len);
    _frame_len = len;
    _frame_ready = true;
  }
  _rx_error = false;
  uart_reset_rx_len(UART0);
}

static void _uart_error(uart_error_t error)
{
  (void)error;
  _rx_error = true;
}

void mpm_port_init()
{
  uart_device_t dev = {.mode = UART_8E2, .baud_rate = MPM_BAUDRATE};
  uart_init(UART0, &dev);
  uart_callbacks_t cb = {
      .frame_received = _frame_received,
      .error = _uart_error,
  };
  uart_enable_irqs(UART0, &cb);
  uart_enable_rx_irq(UART0, _rx_buf, sizeof(_rx_buf));
  uart_enable_tx_irq(UART0, _tx_buf, sizeof(_tx_buf));
  file_server_init(0, 0);
}

bool mpm_port_read_frame(uint8_t* buf, uint8_t* len)
{
  if (!_frame_ready) return false;
  bool masked = IntDisableMaster();
  memcpy(buf, _frame, _frame_len);
  *len = _frame_len;
  _frame_ready = false;
  if (!masked) IntEnableMaster();
  return true;
}

bool mpm_port_read_ppm(uint16_t* us, uint8_t* n)
{
  (void)us;
  (void)n;
  return false;
}

void mpm_port_write(const uint8_t* data, uint8_t len) { uart_tx_irq(UART0, data, len); }

uint16_t mpm_port_write_free() { return uart_tx_free(UART0); }

void mpm_port_led(bool on) { board_led(BOARD_LED_GREEN, on); }

void mpm_port_reset_rf() { radio_off(); }

// The chip's IEEE address: its first 32 bits. MPM_MODULE_ID (CMake's
// CC27_MODULE_ID): another module's, its receivers bound to this one too
uint32_t mpm_port_module_id()
{
#if defined(MPM_MODULE_ID)
  return MPM_MODULE_ID;
#endif
  uint32_t id;
  memcpy(&id, fcfg->deviceInfo.macAddr, sizeof(id));
  return id;
}

// A file request: the reply as telemetry, when there is room for it
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
  while (uart_tx_free(UART0) < MPM_TELEM_HDR_LEN + n) mpm_poll();
  uart_tx_irq(UART0, frame, MPM_TELEM_HDR_LEN + n);
}

// The log in frames of LOG_FRAME bytes, while there is room for them and
// LOG_RESERVE bytes more for the protocol's telemetry
#define LOG_FRAME 64
#define LOG_RESERVE 64
static void _log_send()
{
  while (log_pending() && uart_tx_free(UART0) >= MPM_TELEM_HDR_LEN + LOG_FRAME + LOG_RESERVE) {
    uint8_t frame[MPM_TELEM_HDR_LEN + LOG_FRAME];
    uint8_t n = log_read((char*)frame + MPM_TELEM_HDR_LEN, LOG_FRAME);
    if (n == 0) break;
    frame[0] = 'M';
    frame[1] = 'P';
    frame[2] = MPM_TELEM_LOG;
    frame[3] = n;
    uart_tx_irq(UART0, frame, MPM_TELEM_HDR_LEN + n);
  }
}

void mpm_port_idle()
{
  radio_poll();
  if (_file_req_ready) _file_request();
  if (log_on) _log_send();
}
