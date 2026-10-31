#pragma once

// The chip's part of the RF core driver (radio.c): rf_cc26x2.c on the
// CC2652, rf_cc26x0.c on the CC2650 (MCU_CC26X0). The RF core firmware and
// its patches, the setup command and its overrides, the modem's frequency
// estimate, HPOSC

#include <stdbool.h>
#include <stdint.h>

#include <driverlib/rf_common_cmd.h>

#include "radio.h"

// RF core symbol rate: rate = 24 MHz * rateWord / (preScale * 2^20)
#define SYMBOL_RATE_PRESCALE 15

// No frequency estimate (rf_chip_foff())
#define FOFF_NONE INT16_MIN

// The Bluetooth LE 2M PHY (radio_phy_t's ble_2m). A constant, so radio.c
// drops its paths on the CC2650
#if defined(MCU_CC26X0)
#define RF_CHIP_BLE_2M false
#else
#define RF_CHIP_BLE_2M true
#endif

// Before the RF core powers up: its firmware (RFCMODESEL)
void rf_chip_mode();

// Once it has booted: the CPE patch (ble: Bluetooth LE 5's, for the 2M PHY)
// and the others, then the RF core's access to the system bus (RX queue in
// RAM)
void rf_chip_patch(bool ble);

// The setup command of phy (ble: the Bluetooth LE 2M one), the extra
// override words (radio_set_extra_overrides()) after the chip's
rfc_radioOp_t* rf_chip_setup(const radio_phy_t* phy, bool ble, const uint32_t* extra,
                             uint8_t n_extra);

// The modem's frequency estimate of the last packet (DEMMAFC0 counts), or
// FOFF_NONE. The modem is clocked only while an RX command runs, and reading
// it with the clock off hangs the bus until a reset: call only while the
// command goes on
int16_t rf_chip_foff();

// HPOSC (CC2652RB): the RF core's HF clock offset, for the temperature.
// Offset 0 without one
void rf_chip_hposc_init();
// Follow the temperature, the RF core must be idle
void rf_chip_hposc_update();
int16_t rf_chip_hposc_offset();
