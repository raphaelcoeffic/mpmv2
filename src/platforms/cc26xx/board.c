// board_init() on the CC26xx (CC2650, CC2652): the 48 MHz oscillator, power
// domains, GPIO, then the board's pins (board_pins_init(), the board's
// board.c)
#include <driverlib/gpio.h>
#include <driverlib/osc.h>
#include <driverlib/prcm.h>
#include "board.h"

#define POWER_DOMAINS (PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL)

void board_init()
{
  CPUcpsid();

  // XOSC_HF (48 MHz xtal or HPOSC) for the RF core
  OSCHF_TurnOnXosc();
  while (!OSCHF_AttemptToSwitchToXosc()) {
  }

  // Power ON periph and serial
  PRCMPowerDomainOn(POWER_DOMAINS);
#if defined(MCU_CC26X0)
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

  board_pins_init();

  // Enable interrupts
  CPUcpsie();
}


