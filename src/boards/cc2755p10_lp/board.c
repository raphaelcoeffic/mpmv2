// LP-EM-CC2755P10's pins: the LEDs, the buttons (the platform's board.c
// sets up the chip and UART0)
#include "board.h"

#include <driverlib/gpio.h>

void board_pins_init(void)
{
  board_ioc(BOARD_LED_RED, IOC_IOC0_PORTCFG_BASE);
  board_ioc(BOARD_LED_GREEN, IOC_IOC0_PORTCFG_BASE);
  GPIOClearDio(BOARD_LED_RED);
  GPIOClearDio(BOARD_LED_GREEN);
  GPIOSetOutputEnableDio(BOARD_LED_RED, GPIO_OUTPUT_ENABLE);
  GPIOSetOutputEnableDio(BOARD_LED_GREEN, GPIO_OUTPUT_ENABLE);

  // Buttons to ground
  board_ioc(BOARD_BTN1, IOC_IOC0_PORTCFG_BASE | IOC_IOC0_INPEN_EN | IOC_IOC0_PULLCTL_PULL_UP);
  board_ioc(BOARD_BTN2, IOC_IOC0_PORTCFG_BASE | IOC_IOC0_INPEN_EN | IOC_IOC0_PULLCTL_PULL_UP);
}

void board_led(uint32_t dio, bool on) { GPIOWriteDio(dio, on ? 1 : 0); }

void board_led_toggle(uint32_t dio) { GPIOToggleDio(dio); }

bool board_button(uint32_t dio) { return GPIOReadDio(dio) == 0; }
