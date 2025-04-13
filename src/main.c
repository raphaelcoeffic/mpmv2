#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/uart.h>

#include <string.h>

#include "board.h"
#include "dma.h"
#include "timer.h"
#include "uart.h"

#include "debug.h"

#if defined(LED_SPI) && defined(LED_DIN)
  #include "led_rgb.h"
#endif

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
  dma_init();
}

static void serial_print(const char* str)
{
  uart_print_irq(UART0, str);
}

static void serial_write(const uint8_t* data, uint32_t len)
{
  uart_tx_irq(UART0, data, len);
}

static void serial_wait_dma() { uart_tx_dma_wait(UART0); }

static void serial_write_dma(const uint8_t* data, uint32_t len)
{
  serial_wait_dma();
  uart_tx_dma(UART0, data, len);
}

static void serial_print_dma(const char* str)
{
  uint32_t len = strlen(str);
  serial_write_dma((const uint8_t*)str, len);
}

static void leds_init()
{
#if defined(LED_GREEN) && defined(LED_RED)
  IOCPinTypeGpioOutput(LED_GREEN);
  IOCPinTypeGpioOutput(LED_RED);
  GPIO_clearDio(LED_GREEN);
  GPIO_clearDio(LED_RED);
#endif

#if defined(LED_SPI) && defined(LED_DIN)
  led_rgb_init(LED_SPI, LED_DIN);
  led_rgb_set_color(LED_SPI, LED_COLOR(0x00, 0x00, 0x60));
#endif
}

static const char lorem_ipsum[] =
    // Section 1.10.32 of "de Finibus Bonorum et Malorum", written by Cicero in 45 BC
    "Sed ut perspiciatis unde omnis iste natus error sit voluptatem \n"
    "accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab\n"
    "illo inventore veritatis et quasi architecto beatae vitae dicta sunt\n"
    "explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut\n"
    "odit aut fugit, sed quia consequuntur magni dolores eos qui ratione\n"
    "voluptatem sequi nesciunt. Neque porro quisquam est, qui dolorem ipsum\n"
    "quia dolor sit amet, consectetur, adipisci velit, sed quia non numquam\n"
    "eius modi tempora incidunt ut labore et dolore magnam aliquam quaerat\n"
    "voluptatem. Ut enim ad minima veniam, quis nostrum exercitationem ullam\n"
    "corporis suscipit laboriosam, nisi ut aliquid ex ea commodi consequatur?\n"
    "Quis autem vel eum iure reprehenderit qui in ea voluptate velit esse quam\n"
    "nihil molestiae consequatur, vel illum qui dolorem eum fugiat quo \n"
    "voluptas nulla pariatur?\n";

int main(void)
{
  board_init();
  timer_init();
  serial_init();
  leds_init();

#if defined(TEST_PIN)
  GPIO_setDio(TEST_PIN);
  IOCPinTypeGpioOutput(TEST_PIN);
#endif

  debugln("\n## Boot completed ##\n");
  serial_print_dma(lorem_ipsum);

  while (true) {
    if (frame_received) {
      debugln("frame recvd (size=%d)", rx_len);

      // Echo RX buffer content
      // serial_write_dma(rx_buf, rx_len);

      // TEST: reply with received len
      uint8_t data = (uint8_t)rx_len;
      serial_write_dma(&data, 1);

      // Reset RX buffer
      uart_reset_rx_len(UART0);
      frame_received = false;
    }
  }
}
