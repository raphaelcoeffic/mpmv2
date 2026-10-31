// The CC2652's part of the RF core driver (rf_chip.h): TI's proprietary or
// Bluetooth LE 5 CPE patch, the proprietary setup (CMD_PROP_RADIO_DIV_SETUP)
// or the Bluetooth LE 2M one, the modem's frequency estimate, HPOSC
#include <driverlib/aon_batmon.h>
#include <driverlib/osc.h>
#include <driverlib/prcm.h>
#include <driverlib/rfc.h>
#include <driverlib/rf_ble_cmd.h>
#include <driverlib/rf_ble_mailbox.h>
#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_prop_cmd.h>
#include <driverlib/rf_prop_mailbox.h>
#include <rf_patches/rf_patch_cpe_bt5.h>
#include <rf_patches/rf_patch_cpe_prop.h>

#include <string.h>

#include "rf_chip.h"
#include "timer.h"

// Modem DEMMAFC0 (CC27xx LRFDMDM name): COMPVAL (7:0), the matched filter
// frequency compensation of the last packet, signed, kept until the next sync
// (docs/rf-core-notes.md)
#define MDM_DEMMAFC0 0x400451C0

// Overrides from the TI radio configuration database
// (cc2652r_prop_pg21, setting tc901: 250 kbps 2-GFSK, 2.4 GHz)
static const uint32_t _base_overrides[] = {
    // DC/DC regulator: In Tx, use DCDCCTL5[3:0]=0x3 (DITHER_EN=0 and IPEAK=3).
    0x00F388D3,
    // Tx: Configure PA ramp time, PACTL2.RC=0x1 (in ADI0, set PACTL2[4:3]=0x1)
    ADI_2HALFREG_OVERRIDE(0, 16, 0x8, 0x8, 17, 0x1, 0x0),
    // Rx: Set AGC reference level to 0x22 (default: 0x2E)
    HW_REG_OVERRIDE(0x609C, 0x0022),
};
#define BASE_OVERRIDES (sizeof(_base_overrides) / sizeof(_base_overrides[0]))

// Base + anti-aliasing filter + sync threshold + HPOSC + extra + end marker
static uint32_t _overrides[BASE_OVERRIDES + 4 + RADIO_EXTRA_OVERRIDES];

// Bluetooth LE 2M PHY under the proprietary packet commands, as TI's wBMS
// setting (cc2652r_ble_pg21, setting_wbms_2m: CMD_BLE5_RADIO_SETUP, then
// CMD_PROP_TX_ADV / CMD_PROP_RX_ADV). Its CRC overrides are left out
static const uint32_t _ble_base_overrides[] = {
    // DC/DC regulator: In Tx, use DCDCCTL5[3:0]=0x3 (DITHER_EN=0 and IPEAK=3).
    0x00F388D3,
    // Bluetooth 5: Set pilot tone length to 20 us Common
    HW_REG_OVERRIDE(0x6024, 0x2E20),
    // Bluetooth 5: Compensate for reduced pilot tone length
    0x01280263,
    // Bluetooth 5: Default to no CTE.
    HW_REG_OVERRIDE(0x5328, 0x0000),
    // Synth: Increase mid code calibration time to 5 us
    0x00058683,
    HW32_ARRAY_OVERRIDE(0x4004, 1),
    0x38183C30,
    // Bluetooth 5: Move synth start code
    HW_REG_OVERRIDE(0x4064, 0x3C),
    // Bluetooth 5: Set DTX gain -5% for 1 Mbps, threshold
    0x00E787E3,
    0x00950803,
    // Bluetooth 5: Set DTX gain -2.5% for 2 Mbps, threshold
    0x00F487F3,
    0x012A0823,
    // Bluetooth 5: Set synth fine code calibration interval
    HW32_ARRAY_OVERRIDE(0x4020, 1),
    0x41005F00,
    // Bluetooth 5: Adapt to synth fine code calibration interval
    0xC0040141,
    0x0007DD44,
    // Bluetooth 5: Set enhanced TX shape
    0x000D8C73,
    // Packet format byte of the CPE parameters (index 0x10, next to the CRC
    // length of TI's wBMS overrides): 0x04 = MSB first, no whitening, as the
    // proprietary setup with bMsbFirst. The Bluetooth setup leaves 0x01: LSB
    // first, whitened with the Bluetooth LFSR seeded 0x65 (measured; 0x07 with
    // the proprietary PN9 whitening)
    0x00048103,
};
#define BLE_BASE_OVERRIDES (sizeof(_ble_base_overrides) / sizeof(_ble_base_overrides[0]))

static uint32_t _ble_2m_overrides[] = {
    // Bluetooth 5: Set pilot tone length to 20 us
    HW_REG_OVERRIDE(0x5320, 0x03C0),
    // Bluetooth 5: Compensate syncTimeAdjust
    0x00F102A3,
    // Bluetooth 5: increase low gain AGC delay for 2 Mbps
    HW_REG_OVERRIDE(0x60A4, 0x7D00),
    // nRF24L01 signal (modulation index 0.32): modem values of TI's NESB
    // 2 Mbps PHY of the CC27xx (nesb_1m_2m_cc27xx.json), whose modem has the
    // same registers and fields (TI's BLE values match on both). Measured on
    // NRF 2 Mbps packets without them: none received, 25% at best with any trim
    // (bit errors in runs of zeros). DEMMISC2 MAFCGAIN 1 (frequency offset
    // tracking): without it, nothing
    HW_REG_OVERRIDE(0x50D4, 0x15F9),
    // Matched filter DEMMAFI0..2: C1C7 3, C2C6 21, C3C5 79, C4 124 (BLE: 4, 32,
    // 123, 193): without it, 97%
    HW_REG_OVERRIDE(0x5100, 0x0300),
    HW_REG_OVERRIDE(0x5104, 0x4F15),
    HW_REG_OVERRIDE(0x5108, 0x007C),
    0xFFFFFFFF,
};

// Base + sync threshold + HPOSC + extra + end marker. The lists are in RAM and all
// four are given, as TI's settings (a const list in flash and NULL lists got
// BLE_ERROR_PAR)
static uint32_t _ble_overrides[BLE_BASE_OVERRIDES + 3 + RADIO_EXTRA_OVERRIDES];
static uint32_t _ble_no_overrides[] = {0xFFFFFFFF};


// HPOSC (BAW oscillator of the CC2652RB): the RF core is told the HF clock
// offset, from the factory calibration and the chip temperature, as the TI
// RF driver does (HPOSC_OVERRIDE at setup, CMD_UPDATE_HPOSC_FREQ afterwards)
#define HPOSC_CHECK_MS 1000
static bool _hposc;
static int16_t _hposc_offset;  // 2^-22 relative
static uint32_t _hposc_check;

static rfc_CMD_PROP_RADIO_DIV_SETUP_t _setup;
static rfc_CMD_BLE5_RADIO_SETUP_t _ble_setup;

void rf_chip_mode()
{
  // All RF modes are 0 on CC13x2/CC26x2: the PHY is chosen by patches and setup
  HWREG(PRCM_BASE + PRCM_O_RFCMODESEL) = RF_MODE_AUTO;
}

void rf_chip_patch(bool ble)
{
  // The patches copy their image once and count on the retained RF core RAM:
  // with two of them, load the whole image every time
  cleanBt5Patch();
  cleanPropPatch();
  if (ble) rf_patch_cpe_bt5();
  else rf_patch_cpe_prop();

  // Allow the RF core to access the system bus (RX queue in RAM)
  RFCDoorbellSendTo(CMDR_DIR_CMD_1BYTE(CMD_BUS_REQUEST, 1));
}

int16_t rf_chip_foff()
{
  if (!(HWREG(RFC_PWR_NONBUF_BASE + RFC_PWR_O_PWMCLKEN) & RFC_PWR_PWMCLKEN_MDM))
    return FOFF_NONE;
  return (int8_t)HWREG(MDM_DEMMAFC0);
}

void rf_chip_hposc_init()
{
  static bool done;
  if (done) return;
  done = true;

  _hposc = OSC_IsHPOSCEnabled();
  if (_hposc) {
    AONBatMonEnable();
    OSC_HPOSCInitializeFrequencyOffsetParameters();
  }
}

static int16_t _hposc_get_offset()
{
  int32_t offset = OSC_HPOSCRelativeFrequencyOffsetGet(AONBatMonTemperatureGetDegC());
  _hposc_check = millis();
  return OSC_HPOSCRelativeFrequencyOffsetToRFCoreFormatConvert(offset);
}

void rf_chip_hposc_update()
{
  if (!_hposc || millis() - _hposc_check < HPOSC_CHECK_MS) return;

  int16_t offset = _hposc_get_offset();
  if (offset == _hposc_offset) return;

  _hposc_offset = offset;
  RFCDoorbellSendTo(CMDR_DIR_CMD_2BYTE(CMD_UPDATE_HPOSC_FREQ, (uint16_t)offset));
}

int16_t rf_chip_hposc_offset() { return _hposc ? _hposc_offset : 0; }

static void _init_setup(const radio_phy_t* phy, const uint32_t* extra, uint8_t n_extra)
{
  memset(&_setup, 0, sizeof(_setup));
  _setup.commandNo = CMD_PROP_RADIO_DIV_SETUP;
  _setup.startTrigger.triggerType = TRIG_NOW;
  _setup.condition.rule = COND_NEVER;

  _setup.modulation.modType = phy->fsk ? 0 : 1;  // 2-FSK or 2-GFSK
  _setup.modulation.deviation = phy->deviation / 250;
  _setup.modulation.deviationStepSz = 0;  // 250 Hz steps

  _setup.symbolRate.preScale = SYMBOL_RATE_PRESCALE;
  _setup.symbolRate.rateWord =
      (uint32_t)(((uint64_t)phy->symbol_rate * SYMBOL_RATE_PRESCALE << 20) / 24000000);

  _setup.rxBw = phy->rx_bw;
  _setup.preamConf.nPreamBytes = phy->preamble_bytes;
  _setup.preamConf.preamMode = 2;  // first preamble bit = first sync word bit
  _setup.formatConf.nSwBits = phy->sync_bits;
  _setup.formatConf.bMsbFirst = 1;
  _setup.formatConf.whitenMode = 0;

  _setup.config.frontEndMode = 0;  // differential
  _setup.config.biasMode = 0;      // internal bias
  _setup.txPower = phy->tx_power;
  _setup.centerFreq = 2440;
  _setup.intFreq = phy->if_freq;
  _setup.loDivider = 0;

  uint32_t n = BASE_OVERRIDES;
  memcpy(_overrides, _base_overrides, sizeof(_base_overrides));
  // Rx: Set anti-aliasing filter bandwidth to 0x5 (in ADI0, set IFAMPCTL3[7:4]=0x5)
  // The narrow filter of the reference PHY only suits slow rates
  if (phy->symbol_rate <= 250000) _overrides[n++] = ADI_HALFREG_OVERRIDE(0, 61, 0xF, 0x5);
  // Rx: sync word correlation threshold (MDM register 0x5114, default 0x1F)
  if (phy->sync_threshold)
    _overrides[n++] = HW_REG_OVERRIDE(0x5114, phy->sync_threshold << 8 | phy->sync_threshold);
  if (_hposc) {
    _hposc_offset = _hposc_get_offset();
    _overrides[n++] = HPOSC_OVERRIDE((uint32_t)(uint16_t)_hposc_offset);
  }
  for (uint8_t i = 0; i < n_extra; i++) _overrides[n++] = extra[i];
  _overrides[n] = 0xFFFFFFFF;
  _setup.pRegOverride = _overrides;
}

static void _init_ble_setup(const radio_phy_t* phy, const uint32_t* extra, uint8_t n_extra)
{
  memset(&_ble_setup, 0, sizeof(_ble_setup));
  _ble_setup.commandNo = CMD_BLE5_RADIO_SETUP;
  _ble_setup.startTrigger.triggerType = TRIG_NOW;
  _ble_setup.condition.rule = COND_NEVER;
  _ble_setup.defaultPhy.mainMode = 1;  // 2 Mbps for the non-BLE commands
  _ble_setup.config.frontEndMode = 0;  // differential
  _ble_setup.config.biasMode = 0;      // internal bias
  _ble_setup.txPower = phy->tx_power;

  uint32_t n = BLE_BASE_OVERRIDES;
  memcpy(_ble_overrides, _ble_base_overrides, sizeof(_ble_base_overrides));
  if (phy->sync_threshold)
    _ble_overrides[n++] = HW_REG_OVERRIDE(0x5114, phy->sync_threshold << 8 | phy->sync_threshold);
  if (_hposc) {
    _hposc_offset = _hposc_get_offset();
    _ble_overrides[n++] = HPOSC_OVERRIDE((uint32_t)(uint16_t)_hposc_offset);
  }
  // The common list comes before the 2 Mbps one: the extra words can't change
  // what the latter sets
  for (uint8_t i = 0; i < n_extra; i++) _ble_overrides[n++] = extra[i];
  _ble_overrides[n] = 0xFFFFFFFF;
  _ble_setup.pRegOverrideCommon = _ble_overrides;
  _ble_setup.pRegOverride1Mbps = _ble_no_overrides;
  _ble_setup.pRegOverride2Mbps = _ble_2m_overrides;
  _ble_setup.pRegOverrideCoded = _ble_no_overrides;
}

rfc_radioOp_t* rf_chip_setup(const radio_phy_t* phy, bool ble, const uint32_t* extra,
                             uint8_t n_extra)
{
  if (ble) {
    _init_ble_setup(phy, extra, n_extra);
    return (rfc_radioOp_t*)&_ble_setup;
  }
  _init_setup(phy, extra, n_extra);
  return (rfc_radioOp_t*)&_setup;
}
