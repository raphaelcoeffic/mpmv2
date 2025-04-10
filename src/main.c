#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/uart.h>

#include "timer.h"
#include "uart.h"
#include "led_rgb.h"

#if defined(BOARD_CC2652R1_LAUNCHXL)
  #define SERIAL_RX_IOD IOID_2
  #define SERIAL_TX_IOD IOID_3
  #define LED_GREEN     IOID_7
  #define LED_RED       IOID_6
#else
  // *****************
  // * MPM v2 pinout *
  // *****************
  // 
  // DIO_3  <-> USB TX
  // DIO_2  <-> SPORT RX / USB RX
  // DIO_21 <-> SPORT TX
  // DIO_23 <-> SPORT RX INV
  // DIO_24 <-> SPORT TX INV
  // 
  // DIO_22 <-> CPPM
  // 
  #define SERIAL_RX_IOD IOID_2
  #define SERIAL_TX_IOD IOID_3
  #define LED_DIN       IOID_18
  #define LED_SPI       SPI1
  // timing test
  #define LED_GREEN     IOID_7
#endif

#define SERIAL_BAUDRATE 115200

#define POWER_DOMAINS (PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL)

static void board_init()
{
  CPUcpsid();

#if defined(USE_XOSC)
  // Turn XOSC ON
  OSCHF_TurnOnXosc();
  while (!OSCHF_AttemptToSwitchToXosc()) {
  }
#endif

  // Power ON periph and serial
  PRCMPowerDomainOn(POWER_DOMAINS);
  while (PRCMPowerDomainsAllOn(POWER_DOMAINS) != PRCM_DOMAIN_POWER_ON) {
  }

  PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
  PRCMLoadSet();

  // Enable interrupts
  CPUcpsie();
}

// must be a power of 2
#define RX_BUFFER_SIZE 128
#define TX_BUFFER_SIZE 128

static uint8_t rx_buf[RX_BUFFER_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;

static uint8_t tx_buf[TX_BUFFER_SIZE];
static volatile uint32_t tx_head;
static volatile uint32_t tx_tail;

static inline bool _serial_tx_buffer_push(uint8_t data)
{
  // check if buffer is full
  if (((tx_head + 1) & (TX_BUFFER_SIZE - 1)) == tx_tail) {
    return false;
  }

  tx_buf[tx_head] = data;
  tx_head = (tx_head + 1) & (TX_BUFFER_SIZE - 1);
  return true;
}

static inline bool _serial_tx_buffer_pull(uint8_t* data)
{
  // end of buffer
  if (tx_tail == tx_head) return false;

  *data = tx_buf[tx_tail];
  tx_tail = (tx_tail + 1) & (TX_BUFFER_SIZE - 1);
  return true; 
}

static inline bool _serial_rx_buffer_available()
{
  return (rx_tail != rx_head);
}

static inline uint8_t _serial_rx_buffer_read()
{
  // end of buffer
  if (rx_tail == rx_head) return 0;

  uint8_t data = rx_buf[rx_tail];
  rx_tail = (rx_tail + 1) & (RX_BUFFER_SIZE - 1);
  return data;
}

static void _serial_rx_irq(uint8_t data)
{
  // check if buffer is full
  if (((rx_head + 1) & (RX_BUFFER_SIZE - 1)) == rx_tail) {
    // overflow !!!
    return;
  }

  rx_buf[rx_head] = data;
  rx_head = (rx_head + 1) & (RX_BUFFER_SIZE - 1);
}

static uint8_t _serial_tx_irq(uint8_t* data)
{
  return _serial_tx_buffer_pull(data);
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

  uart_callbacks_t uart0_cb = {
    .received = _serial_rx_irq,
    .send = _serial_tx_irq,
    .error = 0,
  };
  uart_enable_irqs(UART0, &uart0_cb);
}

static inline bool serial_read(uint8_t* data)
{
  if (_serial_rx_buffer_available()) {
    *data = _serial_rx_buffer_read();
    return true;
  }

  if (uart_rx_fifo_available(UART0)) {
    *data = uart_get_char(UART0);
    return true;
  }

  return false;
}

static inline bool serial_write(uint8_t data)
{
  if (!uart_tx_irq_enabled(UART0) && !uart_tx_fifo_full(UART0)) {
    uart_put_char(UART0, data);
    return true;
  }

  _serial_tx_buffer_push(data);
  uart_enable_tx_irq(UART0);

  return true;
}

static bool serial_print(const char* str)
{
  while (*str) {
    if (!serial_write(*str)) return false;
    ++str;
  }
  return true;
}

static void leds_init()
{
#if defined(BOARD_CC2652R1_LAUNCHXL)
  IOCPinTypeGpioOutput(LED_GREEN);
  IOCPinTypeGpioOutput(LED_RED);
  GPIO_clearDio(LED_GREEN);
  GPIO_clearDio(LED_RED);
#else
  // IOCPinTypeGpioOutput(LED_GREEN);
  // GPIO_clearDio(LED_GREEN);
  led_rgb_init(LED_SPI, LED_DIN);
#endif
}

int main(void)
{
  board_init();
  timer_init();
  serial_init();
  leds_init();

  serial_print("Boot completed\n");

  // uint32_t next_ms = millis() + 500;
  uint32_t next_tick = get_ticks() + us2ticks(500);

  while (true) {
    // if (millis_after(next_ms)) {
    //   next_ms += 500;
    //   serial_print("Hello world, let's fill that ugly UART FIFO!\n");
    // }

    if (ticks_after(next_tick)) {
      next_tick += us2ticks(500);
#if defined(BOARD_CC2652R1_LAUNCHXL)
      GPIO_toggleDio(LED_GREEN);
#else
      // GPIO_setDio(LED_GREEN);
      led_rgb_set_color(LED_SPI, LED_COLOR(0x12, 0x34, 0x56));
      // GPIO_clearDio(LED_GREEN);
#endif
    }
  }
}
