#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/uart.h>

#include "timer.h"
#include "uart.h"
#include "led_rgb.h"
#include "board.h"

#define SERIAL_BAUDRATE 115200

#define RX_BUFFER_SIZE 128
#define TX_BUFFER_SIZE 128 // must be a power of 2

static uint8_t rx_buf[RX_BUFFER_SIZE];
static uint8_t tx_buf[TX_BUFFER_SIZE];

// Frame received flag
static volatile bool frame_received = false;
static volatile uint32_t rx_len = 0;

static void _serial_rx_timeout()
{
  rx_len = uart_get_rx_len(UART0);
  frame_received = true;
}

static void serial_init()
{
  uart_device_t uart0 = {
    .mode = UART_8N1,
    .baud_rate = SERIAL_BAUDRATE,
    .rx = SERIAL_RX_IOD,
    .tx = SERIAL_TX_IOD,
    .cts = IOID_UNUSED,
    .rts = IOID_UNUSED,
  };
  uart_init(UART0, &uart0);

  // UART IRQ callbacks
  uart_callbacks_t uart0_cb = {
    .frame_received = _serial_rx_timeout,
    .error = 0, // TODO
  };
  uart_enable_irqs(UART0, &uart0_cb);
  uart_enable_rx_irq(UART0, rx_buf, RX_BUFFER_SIZE);
  uart_enable_tx_irq(UART0, tx_buf, TX_BUFFER_SIZE);
}

static void serial_print(const char* str)
{
  uart_print_irq(UART0, str);
}

static void leds_init()
{
#if defined(BOARD_CC2652R1_LAUNCHXL)
  IOCPinTypeGpioOutput(LED_GREEN);
  IOCPinTypeGpioOutput(LED_RED);
  GPIO_clearDio(LED_GREEN);
  GPIO_clearDio(LED_RED);
#else
  led_rgb_init(LED_SPI, LED_DIN);
  led_rgb_set_color(LED_SPI, LED_COLOR(0x00, 0x00, 0x60));
#endif
}

int main(void)
{
  board_init();
  timer_init();
  serial_init();
  leds_init();

#if defined(TEST_PIN)
  IOCPinTypeGpioOutput(TEST_PIN);
  GPIO_setDio(TEST_PIN);
#endif

  serial_print("Boot completed\n");
  serial_print("Test application to test "
               "IRQ based UART sending and receiving\n");

  while (true) {
    if (frame_received) {
      // Echo RX buffer content
      // serial_write_buffer(rx_buf, rx_len);

      // TEST: reply with received len
      uint8_t data = (uint8_t)rx_len;
      uart_tx_irq(UART0, &data, 1);

      // Reset RX buffer
      uart_reset_rx_len(UART0);
      frame_received = false;
    }
  }
}
