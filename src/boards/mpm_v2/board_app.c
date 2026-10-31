// What the applications set up on top of board_init(), not the boot loader:
// the LEDs (the LaunchPad's red and green, the MPM v2's RGB LED dim blue)
#include "board.h"

#if defined(LED_GREEN) && defined(LED_RED)
  #include <driverlib/gpio.h>
  #include <driverlib/ioc.h>
#endif
#if defined(LED_SPI) && defined(LED_DIN)
  #include "led_rgb.h"
#endif

void board_app_init()
{
#if defined(LED_GREEN) && defined(LED_RED)
  IOCPinTypeGpioOutput(LED_GREEN);
  IOCPinTypeGpioOutput(LED_RED);
  GPIO_clearDio(LED_GREEN);
  GPIO_clearDio(LED_RED);
#endif

#if defined(LED_SPI) && defined(LED_DIN)
  led_rgb_init(LED_SPI, LED_DIN);
  led_rgb_set_color(LED_COLOR(0x00, 0x00, 0x60));
#endif
}
