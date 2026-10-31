// XJT-Lite pins at boot (board_init(), platforms/cc26xx/board.c)
#include <driverlib/gpio.h>
#include "board.h"

// Set the level before enabling the output: no glitch
static inline void _gpio_output(uint32_t pin, uint32_t level)
{
  GPIO_writeDio(pin, level);
  IOCPinTypeGpioOutput(pin);
}

// Pins are floating after reset: give a defined level to the
// external parts until their drivers take over
void board_pins_init()
{
  // Front end asleep (CSD = 0) until the radio powers up. LNA and PA from
  // the RF core: CPS = RFC_GPO0 (high during RX), CTX = RFC_GPO1 (high
  // during TX), both low while the RF core is off
  _gpio_output(FEM_CSD, 0);
  IOCPortConfigureSet(FEM_CPS, IOC_PORT_RFC_GPO0, IOC_STD_OUTPUT);
  IOCPortConfigureSet(FEM_CTX, IOC_PORT_RFC_GPO1, IOC_STD_OUTPUT);

  // S.Port released: the buffer's output off, its input (UART TX) idle.
  // FrSky's boot loader leaves UART0 on DIO_29/30: S.Port in back to a
  // plain input, or it stays muxed to UART0 RX next to MODULE_RX
  _gpio_output(SPORT_TX_EN_N, 1);
  _gpio_output(SPORT_TX, 1);
  IOCPinTypeGpioInput(SPORT_RX);

  _gpio_output(LED_RED, 0);
  _gpio_output(LED_GREEN, 0);
  _gpio_output(LED_UNKNOWN, 0);
}
