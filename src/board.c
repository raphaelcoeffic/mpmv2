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
  while (PRCMPowerDomainsAllOn(POWER_DOMAINS) != PRCM_DOMAIN_POWER_ON) {
  }

  PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
  PRCMLoadSet();
  while (!PRCMLoadGet()) {
  }

  _pins_init();

  // Enable interrupts
  CPUcpsie();
}


