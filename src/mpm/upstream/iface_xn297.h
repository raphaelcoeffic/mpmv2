#pragma once

// The XN297 emulation of the port (mpm/multi/xn297.h), for the protocol
// copies that include iface_xn297.h. The chips' interfaces as upstream's:
// MJXq's H26D / H26WH write the NRF24L01 directly
#if defined(CC2500_INSTALLED)
#include "iface_cc2500.h"
#endif
#if defined(NRF24L01_INSTALLED)
#include "iface_nrf24l01.h"
#endif
#include "xn297.h"
