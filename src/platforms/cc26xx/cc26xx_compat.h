#pragma once

// CC26x2 driverlib calls the CC26x0 driverlib (MCU_CC26X0) doesn't have, on
// its own calls

#include <driverlib/prcm.h>
#include <driverlib/rfc.h>

#if defined(MCU_CC26X0)
// PRCM_DOMAIN_POWER_ON when all the domains are on
static inline uint32_t PRCMPowerDomainsAllOn(uint32_t domains)
{
  return PRCMPowerDomainStatus(domains);
}

// PRCM_DOMAIN_POWER_OFF when the domain is off. One domain only:
// PRCMPowerDomainStatus() says off as soon as one of them is
static inline uint32_t PRCMPowerDomainsAllOff(uint32_t domain)
{
  return PRCMPowerDomainStatus(domain);
}

static inline void RFCCpe0IntSelect(uint32_t mask)
{
  HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEISL) &= ~mask;
}
#endif
