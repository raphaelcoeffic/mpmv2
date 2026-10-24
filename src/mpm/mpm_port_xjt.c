// MULTI-Module core on the FrSky XJT-Lite: one link with the radio on UART0,
// 100000 baud 8E2. In on the bay's data pin (the MT12's PPM pin), through
// the board's inverter to MODULE_RX: idle high on the pin (EdgeTX's
// ETX_Pol_Inverted, as on the MPM v2), so inverted back here. Telemetry out
// on S.Port through the board's inverting buffer (SPORT_TX, enabled by
// SPORT_TX_EN_N).
//
// S.Port stays released (board_init()) until the first MULTI frame, then is
// driven: idle low (the UART's idle high, inverted by the buffer: the S.Port
// EdgeTX reads, ETX_Pol_Normal) unless the frame's telemetry invert bit is
// clear (EdgeTX: an internal module), as the STM32 MULTI-Module switches its
// TX_INV. No PPM input, no USB, no external flash: the EEPROM is RAM only.
#include "mpm_port.h"

#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <inc/hw_fcfg1.h>
#include <inc/hw_memmap.h>
#include <inc/hw_types.h>
#include <string.h>

#include "board.h"
#include "mpm.h"
#include "mpm_frame.h"
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

// S.Port: -1 released, else inverted by the pin (telemetry not inverted)
static int8_t _telem_pin_inv = -1;

uint32_t mpm_port_micros() { return micros(); }
uint32_t mpm_port_millis() { return millis(); }

// RX timeout: the radio sends a frame in one go, at least 3 bytes of silence
// (32 bits) after it. Frames with UART errors are dropped, as the MPM does
static void _frame_received()
{
  uint32_t len = uart_get_rx_len(UART0);
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

// The released line reads as a break to an idle low receiver: S.Port stays
// idle TELEM_IDLE_US before the first byte, else the radio misses its start
// bit (as on the MPM v2)
#define TELEM_IDLE_US 500
static void _telem_invert(bool invert)
{
  bool pin_inv = !invert;
  if (_telem_pin_inv == pin_inv) return;
  IOCPortConfigureSet(SPORT_TX, IOC_PORT_MCU_UART0_TX,
                      IOC_STD_OUTPUT | (pin_inv ? IOC_IOMODE_INV : IOC_IOMODE_NORMAL));
  GPIO_clearDio(SPORT_TX_EN_N);
  _telem_pin_inv = pin_inv;
  uint32_t t = micros();
  while (micros() - t < TELEM_IDLE_US) {}
}

void mpm_port_init()
{
  // TX on no pin until the first frame: SPORT_TX stays the GPIO of
  // board_init()
  uart_device_t dev = {
      .mode = UART_8E2,
      .baud_rate = MPM_BAUDRATE,
      .rx = MODULE_RX,
      .tx = IOID_UNUSED,
      .cts = IOID_UNUSED,
      .rts = IOID_UNUSED,
  };
  uart_init(UART0, &dev);
  IOCIOModeSet(MODULE_RX, IOC_IOMODE_INV);
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
  bool masked = IntMasterDisable();
  memcpy(buf, _frame, _frame_len);
  *len = _frame_len;
  _frame_ready = false;
  if (!masked) IntMasterEnable();
  // V1 frames (no flags byte): inverted, a radio's external bay
  mpm_frame_t f;
  if (mpm_frame_decode(buf, *len, &f))
    _telem_invert(*len <= MPM_FRAME_V1_LEN || (buf[MPM_FRAME_V1_LEN] & MPM_FRAME_TELEM_INVERT));
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

void mpm_port_led(bool on) { GPIO_writeDio(LED_GREEN, on); }

void mpm_port_reset_rf() { radio_off(); }

// The chip's IEEE 802.15.4 address: its last 32 bits
uint32_t mpm_port_module_id() { return HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_0); }

void mpm_port_idle() {}
