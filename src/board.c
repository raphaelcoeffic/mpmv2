#include <driverlib/gpio.h>
#include <driverlib/osc.h>
#include <driverlib/prcm.h>
#include "board.h"

#define POWER_DOMAINS (PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL)

// Set the level before enabling the output: no glitch. Unused on a board
// without any of the pins below (LaunchPad)
__attribute__((unused)) static inline void _gpio_output(uint32_t pin, uint32_t level)
{
  GPIO_writeDio(pin, level);
  IOCPinTypeGpioOutput(pin);
}

// Pins are floating after reset: give a defined level to the
// external parts until their drivers take over
static void _pins_init()
{
#if defined(SX1281_NSS)
  // SX1281 shares the SPI bus with the flash: deselected and held in reset
  _gpio_output(SX1281_NSS, 1);
  _gpio_output(SX1281_NRST, 0);
#endif

#if defined(RF_SW)
  // CC2652 to the PA/LNA
  _gpio_output(RF_SW, RF_SW_CC2652);
#endif

#if defined(PA_CTX)
  // TX/RX driven by the RF core PA enable (RFC_GPO1, mapped to CPEGPO1
  // by the radio CPU at boot): high during TX only, low (RX) while off
  IOCPortConfigureSet(PA_CTX, IOC_PORT_RFC_GPO1, IOC_STD_OUTPUT);
#endif

#if defined(ANT_SEL)
  _gpio_output(ANT_SEL, 0);
#endif

#if defined(FEM_CSD)
  // Front end asleep (CSD = 0) until the radio powers up. LNA and PA from
  // the RF core: CPS = RFC_GPO0 (high during RX), CTX = RFC_GPO1 (high
  // during TX), both low while the RF core is off
  _gpio_output(FEM_CSD, 0);
  IOCPortConfigureSet(FEM_CPS, IOC_PORT_RFC_GPO0, IOC_STD_OUTPUT);
  IOCPortConfigureSet(FEM_CTX, IOC_PORT_RFC_GPO1, IOC_STD_OUTPUT);
#endif

#if defined(SPORT_TX_EN_N)
  // S.Port released: the buffer's output off, its input (UART TX) idle.
  // FrSky's boot loader leaves UART0 on DIO_29/30: S.Port in back to a
  // plain input, or it stays muxed to UART0 RX next to MODULE_RX
  _gpio_output(SPORT_TX_EN_N, 1);
  _gpio_output(SPORT_TX, 1);
  IOCPinTypeGpioInput(SPORT_RX);
#endif

#if defined(LED_UNKNOWN)
  _gpio_output(LED_RED, 0);
  _gpio_output(LED_GREEN, 0);
  _gpio_output(LED_UNKNOWN, 0);
#endif

#if defined(MODULE_TX)
  // SPORT TX released (TX ^ TX_INV = 1: R22 pulls the line up),
  // SPORT RX not inverted
  _gpio_output(MODULE_TX_INV, 0);
  _gpio_output(MODULE_TX, 1);
  _gpio_output(MODULE_RX_INV, 0);
#endif
}

void board_init()
{
  CPUcpsid();

  // XOSC_HF (48 MHz xtal or HPOSC) for the RF core
  OSCHF_TurnOnXosc();
  while (!OSCHF_AttemptToSwitchToXosc()) {
  }

  // Power ON periph and serial
  PRCMPowerDomainOn(POWER_DOMAINS);
#if defined(BOARD_XJT_LITE)
  // CC26x0 driverlib: no PRCMPowerDomainsAllOn()
  while (PRCMPowerDomainStatus(POWER_DOMAINS) != PRCM_DOMAIN_POWER_ON) {
  }
#else
  while (PRCMPowerDomainsAllOn(POWER_DOMAINS) != PRCM_DOMAIN_POWER_ON) {
  }
#endif

  PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
  PRCMLoadSet();
  while (!PRCMLoadGet()) {
  }

  _pins_init();

  // Enable interrupts
  CPUcpsie();
}


