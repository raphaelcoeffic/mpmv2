// MULTI-Module core on the LP-EM-CC2755P10: the link with the radio on
// UART0, 100000 baud 8E2, through the XDS110 back channel (a host plays the
// radio: tools/bench/radio_sim.py), normal polarity. No PPM input, no file
// system: the EEPROM in RAM (eeprom.c).
//
// RX in the UART interrupt: a frame ends at the receive timeout (32 bit
// times of silence with bytes left in the FIFO), or, when the interrupt
// emptied the FIFO with the frame's last byte, FRAME_GAP_US after it (seen
// by mpm_port_read_frame()). Frames with UART errors are dropped, as the MPM
// does. TX from a ring, refilled by the interrupt.
#include "mpm/mpm_port.h"

#include <string.h>

#include <driverlib/interrupt.h>
#include <driverlib/uart.h>
#include <inc/hw_fcfg.h>
#include <inc/hw_ints.h>
#include <inc/hw_memmap.h>
#include <inc/hw_types.h>
#include <inc/hw_uart.h>

#include "board.h"
#include "eeprom.h"
#include "mpm/mpm.h"
#include "mpm/mpm_frame.h"
#include "radio.h"
#include "timer.h"

#define MPM_BAUDRATE 100000
#define UART_CLK 48000000
#define UART_ERRORS (UART_INT_OE | UART_INT_BE | UART_INT_PE | UART_INT_FE)
// More than the receive timeout (320 us at 100000 baud)
#define FRAME_GAP_US 400

static uint8_t _rx_buf[64];
static volatile uint8_t _rx_n;
static volatile bool _rx_error;
static volatile uint32_t _rx_last_us;

static uint8_t _frame[MPM_FRAME_MAX_LEN];
static uint8_t _frame_len;
static volatile bool _frame_ready;

static uint8_t _tx_buf[256];  // a ring of 256: 8-bit indexes
static volatile uint8_t _tx_head, _tx_tail;

uint32_t mpm_port_micros() { return micros(); }
uint32_t mpm_port_millis() { return millis(); }

// In the interrupt, or with it masked
static void _frame_end()
{
  if (_rx_n && !_rx_error && _rx_n <= MPM_FRAME_MAX_LEN) {
    memcpy(_frame, _rx_buf, _rx_n);
    _frame_len = _rx_n;
    _frame_ready = true;
  }
  _rx_n = 0;
  _rx_error = false;
}

static void _tx_fill()
{
  while (_tx_tail != _tx_head && !(HWREG(UART0_BASE + UART_O_FR) & UART_FR_TXFF))
    HWREG(UART0_BASE + UART_O_DR) = _tx_buf[_tx_tail++];
  if (_tx_tail == _tx_head) UARTDisableInt(UART0_BASE, UART_INT_TX);
}

static void _uart_isr()
{
  uint32_t status = UARTIntStatus(UART0_BASE, true);
  UARTClearInt(UART0_BASE, status);
  if (status & UART_ERRORS) _rx_error = true;
  if (status & (UART_INT_RX | UART_INT_RT)) {
    while (!(HWREG(UART0_BASE + UART_O_FR) & UART_FR_RXFE)) {
      uint32_t data = HWREG(UART0_BASE + UART_O_DR);
      if (data & (UART_DR_OE_M | UART_DR_BE_M | UART_DR_PE_M | UART_DR_FE_M)) _rx_error = true;
      if (_rx_n < sizeof(_rx_buf)) _rx_buf[_rx_n++] = data;
      else _rx_error = true;
    }
    _rx_last_us = micros();
    if (status & UART_INT_RT) _frame_end();
  }
  if (status & UART_INT_TX) _tx_fill();
}

void mpm_port_init()
{
  board_uart_init(MPM_BAUDRATE);
  UARTDisable(UART0_BASE);
  UARTConfigSetExpClk(UART0_BASE, UART_CLK, MPM_BAUDRATE,
                      UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_TWO | UART_CONFIG_PAR_EVEN);
  UARTSetFifoLevel(UART0_BASE, UART_FIFO_TX2_8, UART_FIFO_RX4_8);
  UARTEnableFifo(UART0_BASE);
  UARTClearInt(UART0_BASE, 0xFFFFFFFFU);
  UARTEnableInt(UART0_BASE, UART_INT_RX | UART_INT_RT | UART_ERRORS);
  IntRegister(INT_UART0_COMB, _uart_isr);
  IntEnable(INT_UART0_COMB);
  UARTEnable(UART0_BASE);
}

bool mpm_port_read_frame(uint8_t* buf, uint8_t* len)
{
  bool masked = IntDisableMaster();
  if (!_frame_ready && _rx_n && micros() - _rx_last_us > FRAME_GAP_US &&
      (HWREG(UART0_BASE + UART_O_FR) & UART_FR_RXFE))
    _frame_end();
  bool ready = _frame_ready;
  if (ready) {
    memcpy(buf, _frame, _frame_len);
    *len = _frame_len;
    _frame_ready = false;
  }
  if (!masked) IntEnableMaster();
  return ready;
}

bool mpm_port_read_ppm(uint16_t* us, uint8_t* n)
{
  (void)us;
  (void)n;
  return false;
}

void mpm_port_write(const uint8_t* data, uint8_t len)
{
  if (len > mpm_port_write_free()) return;
  bool masked = IntDisableMaster();
  while (len--) _tx_buf[_tx_head++] = *data++;
  _tx_fill();
  if (_tx_tail != _tx_head) UARTEnableInt(UART0_BASE, UART_INT_TX);
  if (!masked) IntEnableMaster();
}

uint16_t mpm_port_write_free() { return sizeof(_tx_buf) - 1 - (uint8_t)(_tx_head - _tx_tail); }

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
