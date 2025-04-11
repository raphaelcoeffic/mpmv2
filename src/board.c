#include <driverlib/osc.h>
#include <driverlib/prcm.h>
#include "board.h"

#define POWER_DOMAINS (PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL)

void board_init()
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


