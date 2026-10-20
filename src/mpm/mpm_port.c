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
#include "mpm_frame.h"
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
  uint8_t rx_buf[64];
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

uint32_t mpm_port_micros() { return micros(); }
uint32_t mpm_port_millis() { return millis(); }

// RX timeout: the radio sends a frame in one go, at least 3 bytes of silence
// (32 bits) after it. Frames with UART errors are dropped, as the MPM does
static void _frame_received(uint8_t i)
{
  link_t* l = &_links[i];
  uint32_t len = uart_get_rx_len(l->uart);
  if (!l->rx_error && len <= MPM_FRAME_MAX_LEN) {
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
static int8_t _bay_invert = -1;  // S.Port: -1 released, else TX1_INV

static void _frame_received_bay() { _frame_received(LINK_BAY); }
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

static void _bay_init()
{
  // TX on a pin with nothing on it until the first frame: MODULE_TX stays
  // the GPIO of board_init() (S.Port released)
  _link_init(LINK_BAY, MODULE_RX, IOID_UNUSED, _frame_received_bay, _uart_error_bay);
  IOCPortConfigureSet(MODULE_RX, IOC_PORT_MCU_UART1_RX, IOC_STD_INPUT | IOC_IOMODE_INV);
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
  if (!_frame_ready) return false;
  bool masked = IntMasterDisable();
  memcpy(buf, _frame, _frame_len);
  *len = _frame_len;
  _link = _frame_link;
  _frame_ready = false;
  if (!masked) IntMasterEnable();
#if defined(MODULE_RX)
  // V1 frames (no flags byte): inverted, a radio's external bay
  if (_link == LINK_BAY)
    _bay_telem_invert(*len <= MPM_FRAME_V1_LEN || (buf[MPM_FRAME_V1_LEN] & MPM_FRAME_TELEM_INVERT));
#endif
  return true;
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
