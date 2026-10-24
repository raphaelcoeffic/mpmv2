// XJT-Lite bring-up: what a radio sends on the bay's data pin (MULTI
// serial, 100000 8E2, idle high on the pin) comes back on S.Port, one frame
// at a time (a 1 ms gap ends a frame). The red LED blinks every 500 ms, the
// green one toggles with each frame. Checks the board: clocks, timers, the inverted
// input on DIO_26, the S.Port buffer and its enable.

#include <driverlib/gpio.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/uart.h>

#include "board.h"
#include "timer.h"

#define BAUDRATE     100000
#define FRAME_GAP_US 1000

static uint8_t _frame[64];

static void _uart_init()
{
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
  PRCMLoadSet();
  while (!PRCMLoadGet()) {
  }

  // FrSky's boot loader used UART0 for S.Port: start over
  UARTDisable(UART0_BASE);
  UARTIntDisable(UART0_BASE, 0xFFF);
  UARTIntClear(UART0_BASE, 0xFFF);

  // In from the bay's data pin through the board's inverter: inverted
  // again here. Out to S.Port through the inverting buffer: idle low there
  IOCPortConfigureSet(MODULE_RX, IOC_PORT_MCU_UART0_RX, IOC_STD_INPUT | IOC_IOMODE_INV);
  IOCPortConfigureSet(SPORT_TX, IOC_PORT_MCU_UART0_TX, IOC_STD_OUTPUT);

  UARTConfigSetExpClk(UART0_BASE, 48000000, BAUDRATE,
                      UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_TWO | UART_CONFIG_PAR_EVEN);
  UARTFIFOEnable(UART0_BASE);
  UARTEnable(UART0_BASE);
}

static void _sport_send(const uint8_t *buf, uint32_t len)
{
  GPIO_clearDio(SPORT_TX_EN_N);
  for (uint32_t i = 0; i < len; i++) {
    UARTCharPut(UART0_BASE, buf[i]);
  }
  while (UARTBusy(UART0_BASE)) {
  }
  GPIO_setDio(SPORT_TX_EN_N);
}

int main(void)
{
  board_init();
  timer_init();
  _uart_init();

  uint32_t blink = millis();
  uint32_t last_rx = micros();
  uint32_t len = 0;

  for (;;) {
    if (millis_after(blink + 500)) {
      blink += 500;
      GPIO_toggleDio(LED_RED);
    }

    while (UARTCharsAvail(UART0_BASE)) {
      int32_t c = UARTCharGetNonBlocking(UART0_BASE);
      // Errors in bits 8 to 11: drop the byte
      if (c >= 0 && !(c & 0xF00) && len < sizeof(_frame)) {
        _frame[len++] = c;
      }
      last_rx = micros();
    }

    if (len && micros_after(last_rx + FRAME_GAP_US)) {
      _sport_send(_frame, len);
      len = 0;
      GPIO_toggleDio(LED_GREEN);
    }
  }
}
