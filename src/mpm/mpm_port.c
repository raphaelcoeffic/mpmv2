// MULTI-Module core on the board: the radio link on UART0 (LaunchPad: XDS110
// back channel, MPM v2: USB console), 100000 baud 8E2, not inverted. The
// module bay (UART1, inverted) comes later
#include "mpm_port.h"

#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <inc/hw_fcfg1.h>
#include <inc/hw_memmap.h>
#include <inc/hw_types.h>
#include <string.h>

#include "board.h"
#include "mpm_frame.h"
#include "radio.h"
#include "timer.h"
#include "uart.h"

#if defined(LED_GREEN)
  #include <driverlib/gpio.h>
#elif defined(LED_SPI) && defined(LED_DIN)
  #include "led_rgb.h"
  #define LED_ON_COLOR LED_COLOR(0x00, 0x40, 0x00)
#endif

#define MPM_UART UART0
#define MPM_BAUDRATE 100000

static uint8_t _rx_buf[64];
static uint8_t _tx_buf[256];  // power of 2

static uint8_t _frame[MPM_FRAME_MAX_LEN];
static uint8_t _frame_len;
static volatile bool _frame_ready;
static volatile bool _rx_error;

uint32_t mpm_port_micros() { return micros(); }
uint32_t mpm_port_millis() { return millis(); }

// RX timeout: the radio sends a frame in one go, at least 3 bytes of silence
// (32 bits) after it. Frames with UART errors are dropped, as the MPM does
static void _frame_received()
{
  uint32_t len = uart_get_rx_len(MPM_UART);
  if (!_rx_error && len <= MPM_FRAME_MAX_LEN) {
    memcpy(_frame, _rx_buf, len);
    _frame_len = len;
    _frame_ready = true;
  }
  _rx_error = false;
  uart_reset_rx_len(MPM_UART);
}

static void _uart_error(uart_error_t error)
{
  (void)error;
  _rx_error = true;
}

void mpm_port_init()
{
  uart_device_t dev = {
      .mode = UART_8E2,
      .baud_rate = MPM_BAUDRATE,
      .rx = SERIAL_RX_IOD,
      .tx = SERIAL_TX_IOD,
      .cts = IOID_UNUSED,
      .rts = IOID_UNUSED,
  };
  uart_init(MPM_UART, &dev);
  uart_callbacks_t cb = {
      .frame_received = _frame_received,
      .error = _uart_error,
  };
  uart_enable_irqs(MPM_UART, &cb);
  uart_enable_rx_irq(MPM_UART, _rx_buf, sizeof(_rx_buf));
  uart_enable_tx_irq(MPM_UART, _tx_buf, sizeof(_tx_buf));
}

bool mpm_port_read_frame(uint8_t* buf, uint8_t* len)
{
  if (!_frame_ready) return false;
  bool masked = IntMasterDisable();
  memcpy(buf, _frame, _frame_len);
  *len = _frame_len;
  _frame_ready = false;
  if (!masked) IntMasterEnable();
  return true;
}

void mpm_port_write(const uint8_t* data, uint8_t len) { uart_tx_irq(MPM_UART, data, len); }

uint16_t mpm_port_write_free() { return uart_tx_free(MPM_UART); }

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
