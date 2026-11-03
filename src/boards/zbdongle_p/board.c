// ZBDongle-P pins at boot (board_init(), platforms/cc26xx/board.c): the RF
// switch on the normal path. The applications have nothing more to set up
#include <driverlib/gpio.h>
#include "board.h"

void board_pins_init()
{
  // Level first, then the output: no glitch
  GPIO_writeDio(RF_HIGH_PA, 0);
  IOCPinTypeGpioOutput(RF_HIGH_PA);
}

void board_app_init() {}
