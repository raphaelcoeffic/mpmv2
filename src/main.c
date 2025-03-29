#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/uart.h>

#include "timer.h"

// CC2652R1-LAUNCHXL
#define SERIAL_RX_IOD IOID_2
#define SERIAL_TX_IOD IOID_3

#define LED1_IOD IOID_7
#define LED2_IOD IOID_6

#define SERIAL_BAUDRATE 115200

#define POWER_DOMAINS (PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL)

void board_init() {
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
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
  PRCMLoadSet();

  // LEDs
  IOCPinTypeGpioOutput(LED1_IOD);
  IOCPinTypeGpioOutput(LED2_IOD);
  GPIO_clearDio(LED1_IOD);
  GPIO_clearDio(LED2_IOD);

  // Serial
  IOCPinTypeUart(UART0_BASE, SERIAL_RX_IOD, SERIAL_TX_IOD, IOID_UNUSED,
                 IOID_UNUSED);

  UARTDisable(UART0_BASE);
  UARTConfigSetExpClk(UART0_BASE, SysCtrlClockGet(), SERIAL_BAUDRATE,
                      UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                          UART_CONFIG_PAR_NONE);
  UARTEnable(UART0_BASE);

  // Enable interrupts
  CPUcpsie();
}

void uart_print(const char *str) {
  while (*str) {
    UARTCharPut(UART0_BASE, *str);
    str++;
  }
}

int main(void) {
  // Init chip and peripherals
  board_init();

  // Start timer
  timer_init();

  uint32_t ms = 0;
  uint32_t us = 0;

  while (1) {
    uint32_t now_ms = millis();
    uint32_t now_us = micros();

    if (now_ms - ms >= 500) {
      ms = now_ms;
      GPIO_toggleDio(LED1_IOD);
      uart_print("#\n");
    }

    if (now_us - us >= 20000) {
      us = now_us;
      GPIO_toggleDio(LED2_IOD);
    }
  }
}
