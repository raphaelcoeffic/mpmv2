// MULTI-Module core on the LP-EM-CC2755P10: the link with the radio on
// UART0, 100000 baud 8E2, through the XDS110 back channel (a host plays the
// radio: tools/bench/radio_sim.py), normal polarity. No PPM input, no file
// system: the EEPROM in RAM (eeprom.c).
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
#include "mpm/mpm.h"
#include "mpm/mpm_frame.h"
#include "radio.h"
#include "timer.h"
#include "uart.h"

#define MPM_BAUDRATE 100000

static uint8_t _rx_buf[64];   // MULTI frames
static uint8_t _tx_buf[256];  // power of 2
static volatile bool _rx_error;

static uint8_t _frame[MPM_FRAME_MAX_LEN];
static uint8_t _frame_len;
static volatile bool _frame_ready;

uint32_t mpm_port_micros() { return micros(); }
uint32_t mpm_port_millis() { return millis(); }

static void _frame_received()
{
  uint32_t len = uart_get_rx_len(UART0);
  // A full buffer: more than a frame
  if (!_rx_error && len <= MPM_FRAME_MAX_LEN) {
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

void mpm_port_idle() {}
