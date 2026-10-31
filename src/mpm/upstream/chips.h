#pragma once

// The RF chips' interfaces for the emulations (mpm/multi/a7105.c, cc2500.c,
// cyrf6936.c, nrf24l01.c): the copies of iface_*.h here, with the functions
// of *_SPI.ino. The MULTI-Module repo has its own chips.h (mpm/sketch), on
// Multiprotocol/iface_*.h

#include "mpm_compat.h"

#include "iface_a7105.h"
#include "iface_cc2500.h"
#include "iface_cyrf6936.h"
#include "iface_nrf24l01.h"
