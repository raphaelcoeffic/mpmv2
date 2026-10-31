// The CC2650's part of the RF core driver (rf_chip.h): the RF core's
// proprietary 2.4 GHz firmware with TI's generic FSK patches for the command
// processor, the modem and the RF engine, CMD_PROP_RADIO_SETUP (no sub-GHz
// setup, no Bluetooth LE 5 one). No frequency estimate, no HPOSC
#include <driverlib/prcm.h>
#include <driverlib/rfc.h>
#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_prop_cmd.h>
#include <driverlib/rf_prop_mailbox.h>
#include <rf_patches/rf_patch_cpe_genfsk.h>
#include <rf_patches/rf_patch_mce_genfsk.h>
#include <rf_patches/rf_patch_rfe_genfsk.h>

#include <string.h>

#include "rf_chip.h"

// The overrides of FrSky's XJT-Lite firmware (read from its RAM, the same for
// all its PHYs), TI's for the generic FSK patches. The synth's clock and loop
// filter come after the first one, by symbol rate
static const uint32_t _base_overrides[] = {
    // Modem and RF engine from their RAM patches
    MCE_RFE_OVERRIDE(1, 0, 0, 1, 0, 0),
    // Synth: calibration, programming time-out (TI's values)
    HW32_ARRAY_OVERRIDE(0x4004, 1),
    0x1C0C0618,
    0xC00401A1,
    0x21010101,
    0xC0040141,
    0x00214AD3,
    0x03000243,
    0x00008883,
    // ADI0 register 4, low nibble 0xF
    ADI_HALFREG_OVERRIDE(0, 4, 0xF, 0xF),
    0x000188A3,
    // Rx: AGC
    HW_REG_OVERRIDE(0x6088, 0x3F22),
    HW_REG_OVERRIDE(0x608C, 0x0213),
};

// Synth, FrSky's PHYs: FREF 4 MHz (24 MHz / 6), the 24 MHz XOSC as synth
// clock with extra PLL filtering (TI's values)
static const uint32_t _synth_overrides[] = {0x000684A3, 0x02010403};

// Synth, 500 kbps and up (the nRF24L01's 1 Mbps), as TI's CC2650 Bluetooth LE
// settings. With FrSky's, no TX below about 2440 MHz at 1 Mbps (0 packets
// received at any offset, 750 kbps: 10%, 500 kbps: 80 to 100%), the synth
// failing to follow the modulation: without the extra PLL filtering and with
// the wider loop, 88 to 100% from 2402 to 2480 MHz (LaunchPad receiving)
#define FAST_PHY_RATE 500000
static const uint32_t _synth_fast_overrides[] = {
    HW_REG_OVERRIDE(0x4038, 0x0036),  // RTRIM (POTAILRESTRIM) 6
    0x000784A3,                       // FREF 3.43 MHz (24 MHz / 7)
    0xA47E0583,                       // Loop bandwidth after lock: 80 kHz (K2)
    0xEAE00603,                       // (K3, LSB)
    0x00010623,                       // (K3, MSB)
};
// Its deviation, Bluetooth LE's: from 2402 to 2480 MHz, 88 to 100% at 250
// kHz, 56 to 100% at 200 kHz (the high channels), 54 to 82% at 160 kHz, and
// worse above 250 kHz. nRF24L01 receivers take 250 kHz (Bluetooth LE's
// advertising packets are received with them)
#define FAST_PHY_DEVIATION 250000
#define BASE_OVERRIDES (sizeof(_base_overrides) / sizeof(_base_overrides[0]))

// Base + synth + extra + end marker
static uint32_t _overrides[BASE_OVERRIDES + 5 + RADIO_EXTRA_OVERRIDES + 1];

static rfc_CMD_PROP_RADIO_SETUP_t _setup;

void rf_chip_mode()
{
  // The RF core firmware of the proprietary mode
  HWREG(PRCM_BASE + PRCM_O_RFCMODESEL) = RF_MODE_PROPRIETARY_2_4;
}

void rf_chip_patch(bool ble)
{
  (void)ble;
  rf_patch_cpe_genfsk();

  // Allow the RF core to access the system bus (RX queue in RAM)
  RFCDoorbellSendTo(CMDR_DIR_CMD_1BYTE(CMD_BUS_REQUEST, 1));

  // Modem and RF engine RAM clocks on while their patches are written (RF
  // core direct command 0x0607, as TI's RF driver), then back off
  RFCDoorbellSendTo(CMDR_DIR_CMD_2BYTE(0x0607, RFC_PWR_PWMCLKEN_MDMRAM | RFC_PWR_PWMCLKEN_RFERAM));
  rf_patch_mce_genfsk();
  rf_patch_rfe_genfsk();
  RFCDoorbellSendTo(CMDR_DIR_CMD_2BYTE(0x0607, 0));
}

// Not known on the CC26x0 modem
int16_t rf_chip_foff() { return FOFF_NONE; }

// No HPOSC on the CC26x0
void rf_chip_hposc_init() {}

void rf_chip_hposc_update() {}

int16_t rf_chip_hposc_offset() { return 0; }

// CC26x0 RX bandwidth codes: the CC26x2 ones minus 82 (FrSky's PHYs, 70 and
// 31 kbps GFSK: 5 and 3, the CC26x2 codes 87 and 85 that cc2500.c gives them)
#define RX_BW_CC26X2_TO_CC26X0 82

rfc_radioOp_t* rf_chip_setup(const radio_phy_t* phy, bool ble, const uint32_t* extra,
                             uint8_t n_extra)
{
  (void)ble;
  memset(&_setup, 0, sizeof(_setup));
  _setup.commandNo = CMD_PROP_RADIO_SETUP;
  _setup.startTrigger.triggerType = TRIG_NOW;
  _setup.condition.rule = COND_NEVER;

  bool fast = phy->symbol_rate >= FAST_PHY_RATE;
  _setup.modulation.modType = phy->fsk ? 0 : 1;  // 2-FSK or 2-GFSK
  _setup.modulation.deviation = (fast ? FAST_PHY_DEVIATION : phy->deviation) / 250;

  _setup.symbolRate.preScale = SYMBOL_RATE_PRESCALE;
  _setup.symbolRate.rateWord =
      (uint32_t)(((uint64_t)phy->symbol_rate * SYMBOL_RATE_PRESCALE << 20) / 24000000);

  _setup.rxBw = phy->rx_bw - RX_BW_CC26X2_TO_CC26X0;
  _setup.preamConf.nPreamBytes = phy->preamble_bytes;
  _setup.preamConf.preamMode = 2;  // first preamble bit = first sync word bit
  _setup.formatConf.nSwBits = phy->sync_bits;
  _setup.formatConf.bMsbFirst = 1;
  _setup.formatConf.whitenMode = 0;

  _setup.config.frontEndMode = 0;  // differential
  _setup.config.biasMode = 0;      // internal bias
  _setup.txPower = phy->tx_power;

  const uint32_t* synth = fast ? _synth_fast_overrides : _synth_overrides;
  uint32_t n_synth = fast ? sizeof(_synth_fast_overrides) / sizeof(uint32_t)
                          : sizeof(_synth_overrides) / sizeof(uint32_t);
  uint32_t n = 0;
  _overrides[n++] = _base_overrides[0];
  for (uint32_t i = 0; i < n_synth; i++) _overrides[n++] = synth[i];
  for (uint32_t i = 1; i < BASE_OVERRIDES; i++) _overrides[n++] = _base_overrides[i];
  for (uint8_t i = 0; i < n_extra; i++) _overrides[n++] = extra[i];
  _overrides[n] = 0xFFFFFFFF;
  _setup.pRegOverride = _overrides;
  return (rfc_radioOp_t*)&_setup;
}
