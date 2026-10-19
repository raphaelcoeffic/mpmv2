#include <driverlib/aon_batmon.h>
#include <driverlib/interrupt.h>
#include <driverlib/osc.h>
#include <driverlib/prcm.h>
#include <driverlib/rfc.h>
#include <driverlib/rf_ble_cmd.h>
#include <driverlib/rf_ble_mailbox.h>
#include <driverlib/rf_data_entry.h>
#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_prop_cmd.h>
#include <driverlib/rf_prop_mailbox.h>
#include <rf_patches/rf_patch_cpe_bt5.h>
#include <rf_patches/rf_patch_cpe_prop.h>

#include <string.h>

#include "radio.h"
#include "timer.h"

#define CMD_TIMEOUT_US 20000

// RF core symbol rate: rate = 24 MHz * rateWord / (preScale * 2^20)
#define SYMBOL_RATE_PRESCALE 15

// RAT ticks (4 MHz) per us
#define RAT_TICKS_PER_US 4

// RX queue: 4 general entries, packet + appended RSSI
#define RX_ENTRIES 4
#define RX_ENTRY_HDR_LEN 8  // pNextEntry, status, config, length
#define RX_ENTRY_SIZE ((RX_ENTRY_HDR_LEN + RADIO_MAX_PKT_LEN + 1 + 3) & ~3)

// Modem DEMMAFC0 (CC27xx LRFDMDM name): COMPVAL (7:0), the matched filter
// frequency compensation of the last packet, signed, kept until the next sync.
// About 61 counts per deviation, positive when the sender is above the
// receive frequency (docs/rf-core-notes.md)
#define MDM_DEMMAFC0 0x400451C0
#define FOFF_COUNTS_PER_DEVIATION 61
#define FOFF_NONE INT16_MIN

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

static uint32_t _extra_overrides[RADIO_EXTRA_OVERRIDES];
static uint8_t _n_extra_overrides;

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
static bool _ble;  // _ble_setup: Bluetooth LE 2M PHY

static rfc_CMD_FS_t _fs;
static rfc_CMD_PROP_TX_t _tx;
static rfc_CMD_PROP_RX_t _rx;
static rfc_CMD_PROP_TX_ADV_t _tx_adv;
static rfc_CMD_PROP_RX_ADV_t _rx_adv;
static rfc_radioOp_t* _tx_op;  // _tx or _tx_adv
static rfc_radioOp_t* _rx_op;  // _rx or _rx_adv
static rfc_propRxOutput_t _rx_out;

static uint32_t _freq_khz;
static bool _rx_running;

static uint8_t _rx_entries[RX_ENTRIES][RX_ENTRY_SIZE] __attribute__((aligned(4)));
static dataQueue_t _rx_queue;
static uint8_t _rx_next;
static uint16_t _rx_len;
static bool _rx_repeat;
// Frequency estimate of each finished entry, from the RX entry interrupt:
// DEMMAFC0 or FOFF_NONE, _rx_foff_set: entry done
static volatile int16_t _rx_foff[RX_ENTRIES];
static volatile bool _rx_foff_set[RX_ENTRIES];
static uint32_t _foff_deviation;  // Hz, scale of the estimate

static uint32_t _tx_sync;
static uint32_t _rx_sync;
static uint32_t _rx_sync_alt;
static uint16_t _last_status;
static bool _powered;

// Fields written by the RF core must be read as volatile
#define RF_STATUS(op) (*(volatile uint16_t*)&(op)->status)
#define ENTRY_STATUS(e) (*(volatile uint8_t*)&(e)->status)

static bool _cmd_done(uint16_t status) { return status >= DONE_OK; }

static bool _cmd_ok(uint16_t status)
{
  return status == DONE_OK || status == PROP_DONE_OK || status == BLE_DONE_OK;
}

// Submit a radio operation and wait for its end
static radio_status_t _run_op(rfc_radioOp_t* op)
{
  op->status = IDLE;
  uint32_t cmdsta = RFCDoorbellSendTo((uint32_t)op);
  if ((cmdsta & 0xFF) != CMDSTA_Done) {
    _last_status = cmdsta;
    return RADIO_ERR_CMD;
  }

  uint32_t timeout = micros() + CMD_TIMEOUT_US;
  while (!_cmd_done(RF_STATUS(op))) {
    if (micros_after(timeout)) {
      _last_status = RF_STATUS(op);
      return RADIO_ERR_TIMEOUT;
    }
  }

  _last_status = op->status;
  return _cmd_ok(op->status) ? RADIO_OK : RADIO_ERR_CMD;
}

// RF core registers may only be accessed while its power domain is on
static bool _domain_on()
{
  return PRCMPowerDomainsAllOn(PRCM_DOMAIN_RFCORE) == PRCM_DOMAIN_POWER_ON;
}

static void _power_down()
{
  // The interrupt reads RF core registers
  IntDisable(INT_RFC_CPE_0);
  if (_domain_on()) RFCClockDisable();

  PRCMDomainDisable(PRCM_DOMAIN_RFCORE);
  PRCMLoadSet();
  while (!PRCMLoadGet()) {
  }

  PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE);
  while (PRCMPowerDomainsAllOff(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_OFF) {
  }
  _powered = false;
  _rx_running = false;
}

// RX entry done: keep the modem frequency estimate of the packet. The modem
// is clocked only while an RX command runs, and reading it with the clock off
// hangs the bus until a reset: read only during a repeated RX with a free
// entry left, which goes on after the packet (a packet without a free entry
// ends it, as CMD_ABORT, sent after _rx_running = false)
static void _cpe0_isr()
{
  RFCCpeIntClear(IRQ_RX_ENTRY_DONE);

  int free = 0;
  for (int i = 0; i < RX_ENTRIES; i++)
    if (ENTRY_STATUS((rfc_dataEntryGeneral_t*)_rx_entries[i]) != DATA_ENTRY_FINISHED) free++;
  int16_t foff = FOFF_NONE;
  if (_rx_running && _rx_repeat && free && RF_STATUS(_rx_op) == ACTIVE &&
      (HWREG(RFC_PWR_NONBUF_BASE + RFC_PWR_O_PWMCLKEN) & RFC_PWR_PWMCLKEN_MDM))
    foff = (int8_t)HWREG(MDM_DEMMAFC0);

  for (int i = 0; i < RX_ENTRIES; i++) {
    if (ENTRY_STATUS((rfc_dataEntryGeneral_t*)_rx_entries[i]) == DATA_ENTRY_FINISHED &&
        !_rx_foff_set[i]) {
      _rx_foff[i] = foff;
      _rx_foff_set[i] = true;
    }
  }
}

static radio_status_t _power_up(bool ble)
{
  // All RF modes are 0 on CC13x2/CC26x2: the PHY is chosen by patches and setup
  HWREG(PRCM_BASE + PRCM_O_RFCMODESEL) = RF_MODE_AUTO;

  // Start from a powered off RF core: it survives a CPU only reset (debugger)
  _power_down();

  PRCMPowerDomainOn(PRCM_DOMAIN_RFCORE);
  while (PRCMPowerDomainsAllOn(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_ON) {
  }
  PRCMDomainEnable(PRCM_DOMAIN_RFCORE);
  PRCMLoadSet();
  while (!PRCMLoadGet()) {
  }

  HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIFG) = 0;
  RFCClockEnable();

  uint32_t timeout = micros() + CMD_TIMEOUT_US;
  while (!(HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIFG) &
           RFC_DBELL_RFCPEIFG_BOOT_DONE)) {
    if (micros_after(timeout)) return RADIO_ERR_BOOT;
  }
  HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIFG) = 0;

  // Only RX entry done to CPE0 (the other flags are set but not enabled)
  static bool isr;
  if (!isr) {
    isr = true;
    IntRegister(INT_RFC_CPE_0, _cpe0_isr);
  }
  HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIEN) = IRQ_RX_ENTRY_DONE;
  RFCCpe0IntSelect(IRQ_RX_ENTRY_DONE);
  RFCCpeIntClear(IRQ_RX_ENTRY_DONE);
  IntPendClear(INT_RFC_CPE_0);
  IntEnable(INT_RFC_CPE_0);

  // The patches copy their image once and count on the retained RF core RAM:
  // with two of them, load the whole image every time
  cleanBt5Patch();
  cleanPropPatch();
  if (ble) rf_patch_cpe_bt5();
  else rf_patch_cpe_prop();

  // Allow the RF core to access the system bus (RX queue in RAM)
  RFCDoorbellSendTo(CMDR_DIR_CMD_1BYTE(CMD_BUS_REQUEST, 1));

  if ((RFCDoorbellSendTo(CMDR_DIR_CMD(CMD_PING)) & 0xFF) != CMDSTA_Done)
    return RADIO_ERR_BOOT;

  if ((RFCDoorbellSendTo(CMDR_DIR_CMD(CMD_START_RAT)) & 0xFF) != CMDSTA_Done)
    return RADIO_ERR_BOOT;

  _powered = true;
  return RADIO_OK;
}

static void _hposc_init()
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

// Follow the temperature, the RF core must be idle
static void _hposc_update()
{
  if (!_hposc || millis() - _hposc_check < HPOSC_CHECK_MS) return;

  int16_t offset = _hposc_get_offset();
  if (offset == _hposc_offset) return;

  _hposc_offset = offset;
  RFCDoorbellSendTo(CMDR_DIR_CMD_2BYTE(CMD_UPDATE_HPOSC_FREQ, (uint16_t)offset));
}

static void _init_setup(const radio_phy_t* phy)
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
  for (uint8_t i = 0; i < _n_extra_overrides; i++) _overrides[n++] = _extra_overrides[i];
  _overrides[n] = 0xFFFFFFFF;
  _setup.pRegOverride = _overrides;
}

static void _init_ble_setup(const radio_phy_t* phy)
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
  for (uint8_t i = 0; i < _n_extra_overrides; i++) _ble_overrides[n++] = _extra_overrides[i];
  _ble_overrides[n] = 0xFFFFFFFF;
  _ble_setup.pRegOverrideCommon = _ble_overrides;
  _ble_setup.pRegOverride1Mbps = _ble_no_overrides;
  _ble_setup.pRegOverride2Mbps = _ble_2m_overrides;
  _ble_setup.pRegOverrideCoded = _ble_no_overrides;
}

static radio_status_t _run_fs()
{
  memset(&_fs, 0, sizeof(_fs));
  _fs.commandNo = CMD_FS;
  _fs.startTrigger.triggerType = TRIG_NOW;
  _fs.condition.rule = COND_NEVER;
  _fs.frequency = _freq_khz / 1000;
  _fs.fractFreq = (uint16_t)(((_freq_khz % 1000) << 16) / 1000);
  return _run_op((rfc_radioOp_t*)&_fs);
}

radio_status_t radio_init(const radio_phy_t* phy)
{
  if (phy->ble_2m && phy->sync_bits != 32) return RADIO_ERR_PARAM;
  if (phy->deviation > 511750 || phy->sync_bits < 8 || phy->sync_bits > 32 ||
      phy->preamble_bytes == 0 || phy->preamble_bytes > 32 ||
      phy->rx_bw < 64 || phy->rx_bw > 103)
    return RADIO_ERR_PARAM;

  if (_powered) radio_off();
  _hposc_init();

  _ble = phy->ble_2m;
  // Bluetooth LE 2M PHY (500 kHz deviation): measured as 326 kHz
  _foff_deviation = _ble ? 326000 : phy->deviation;
  radio_status_t st = _power_up(_ble);
  if (st != RADIO_OK) return st;

  if (_ble) {
    _init_ble_setup(phy);
    st = _run_op((rfc_radioOp_t*)&_ble_setup);
  } else {
    _init_setup(phy);
    st = _run_op((rfc_radioOp_t*)&_setup);
  }
  if (st == RADIO_OK && _freq_khz) st = _run_fs();
  return st;
}

void radio_off()
{
  if (!_powered) return;
  radio_rx_stop();
  RFCSynthPowerDown();
  _power_down();
}

radio_status_t radio_set_frequency(uint32_t freq_khz)
{
  _freq_khz = freq_khz;
  if (!_powered) return RADIO_OK;  // tuned by radio_init()
  radio_rx_stop();
  _hposc_update();
  return _run_fs();
}

void radio_set_sync_word(uint32_t tx_sync, uint32_t rx_sync)
{
  _tx_sync = tx_sync;
  _rx_sync = rx_sync;
}

void radio_set_rx_sync_alt(uint32_t rx_sync) { _rx_sync_alt = rx_sync; }

static void _init_tx(const uint8_t* data, uint16_t len)
{
  if (len > 255 || _ble) {
    memset(&_tx_adv, 0, sizeof(_tx_adv));
    _tx_adv.commandNo = CMD_PROP_TX_ADV;
    _tx_adv.startTrigger.triggerType = TRIG_NOW;
    _tx_adv.condition.rule = COND_NEVER;
    _tx_adv.pktLen = len;
    _tx_adv.preTrigger.triggerType = TRIG_NOW;  // one preamble, as configured
    _tx_adv.syncWord = _tx_sync;
    _tx_adv.pPkt = (uint8_t*)data;
    _tx_op = (rfc_radioOp_t*)&_tx_adv;
    return;
  }
  memset(&_tx, 0, sizeof(_tx));
  _tx.commandNo = CMD_PROP_TX;
  _tx.startTrigger.triggerType = TRIG_NOW;
  _tx.condition.rule = COND_NEVER;
  _tx.pktConf.bUseCrc = 0;
  _tx.pktConf.bVarLen = 0;
  _tx.pktLen = len;
  _tx.syncWord = _tx_sync;
  _tx.pPkt = (uint8_t*)data;
  _tx_op = (rfc_radioOp_t*)&_tx;
}

radio_status_t radio_send(const uint8_t* data, uint16_t len)
{
  if (!_powered) return RADIO_ERR_PARAM;
  if (len == 0 || len > RADIO_MAX_PKT_LEN) return RADIO_ERR_PARAM;

  radio_rx_stop();
  _init_tx(data, len);
  return _run_op(_tx_op);
}

static void _init_rx_queue(uint16_t len)
{
  for (int i = 0; i < RX_ENTRIES; i++) {
    rfc_dataEntryGeneral_t* e = (rfc_dataEntryGeneral_t*)_rx_entries[i];
    e->pNextEntry = _rx_entries[(i + 1) % RX_ENTRIES];
    e->status = DATA_ENTRY_PENDING;
    e->config.type = DATA_ENTRY_TYPE_GEN;
    e->config.lenSz = 0;
    e->length = len + 1;  // packet + RSSI
    _rx_foff_set[i] = false;
  }
  _rx_queue.pCurrEntry = _rx_entries[0];
  _rx_queue.pLastEntry = 0;  // circular
  _rx_next = 0;
  _rx_len = len;
}

// repeat: continuous reception, else stop after the first packet
static void _init_rx(uint32_t sync_word, uint32_t sync_alt, uint16_t len, bool repeat)
{
  _init_rx_queue(len);
  _rx_repeat = repeat;
  memset(&_rx_out, 0, sizeof(_rx_out));

  // Fixed length: no header, no length field
  if (len > 255 || sync_alt || _ble) {
    memset(&_rx_adv, 0, sizeof(_rx_adv));
    _rx_adv.commandNo = CMD_PROP_RX_ADV;
    _rx_adv.startTrigger.triggerType = TRIG_NOW;
    _rx_adv.condition.rule = COND_NEVER;
    _rx_adv.pktConf.bRepeatOk = repeat;
    _rx_adv.pktConf.bRepeatNok = repeat;
    _rx_adv.rxConf.bAutoFlushIgnored = 1;
    _rx_adv.rxConf.bAppendRssi = 1;
    _rx_adv.syncWord0 = sync_word;
    _rx_adv.syncWord1 = sync_alt;
    _rx_adv.maxPktLen = len;
    _rx_adv.endTrigger.triggerType = TRIG_NEVER;
    _rx_adv.pQueue = &_rx_queue;
    _rx_adv.pOutput = (uint8_t*)&_rx_out;
    _rx_op = (rfc_radioOp_t*)&_rx_adv;
    return;
  }
  memset(&_rx, 0, sizeof(_rx));
  _rx.commandNo = CMD_PROP_RX;
  _rx.startTrigger.triggerType = TRIG_NOW;
  _rx.condition.rule = COND_NEVER;
  _rx.pktConf.bRepeatOk = repeat;
  _rx.pktConf.bRepeatNok = repeat;
  _rx.pktConf.bUseCrc = 0;
  _rx.pktConf.bVarLen = 0;
  _rx.rxConf.bAutoFlushIgnored = 1;
  _rx.rxConf.bAppendRssi = 1;
  _rx.syncWord = sync_word;
  _rx.maxPktLen = len;
  _rx.endTrigger.triggerType = TRIG_NEVER;
  _rx.pQueue = &_rx_queue;
  _rx.pOutput = (uint8_t*)&_rx_out;
  _rx_op = (rfc_radioOp_t*)&_rx;
}

radio_status_t radio_rx_start(uint16_t len)
{
  if (!_powered) return RADIO_ERR_PARAM;
  if (len == 0 || len > RADIO_MAX_PKT_LEN) return RADIO_ERR_PARAM;

  radio_rx_stop();
  _init_rx(_rx_sync, _rx_sync_alt, len, true);

  _rx_op->status = IDLE;
  uint32_t cmdsta = RFCDoorbellSendTo((uint32_t)_rx_op);
  if ((cmdsta & 0xFF) != CMDSTA_Done) {
    _last_status = cmdsta;
    return RADIO_ERR_CMD;
  }
  _rx_running = true;

  // Wait for the command to be running
  uint32_t timeout = micros() + CMD_TIMEOUT_US;
  while (RF_STATUS(_rx_op) < ACTIVE) {
    if (micros_after(timeout)) return RADIO_ERR_TIMEOUT;
  }
  _last_status = RF_STATUS(_rx_op);
  return _last_status == ACTIVE ? RADIO_OK : RADIO_ERR_CMD;
}

radio_status_t radio_send_rx(const uint8_t* data, uint16_t len, uint32_t rx_sync,
                             uint16_t rx_len, uint32_t window_us, radio_packet_t* pkt)
{
  if (!_powered) return RADIO_ERR_PARAM;
  // Short packets only: acknowledgments
  if (len == 0 || len > 255 || rx_len == 0 || rx_len > 255) return RADIO_ERR_PARAM;

  radio_rx_stop();
  _init_tx(data, len);
  _init_rx(rx_sync, 0, rx_len, false);

  // RX right after TX, until a packet or the end of the window (from the RX
  // start)
  _tx_op->condition.rule = COND_STOP_ON_FALSE;
  _tx_op->pNextOp = _rx_op;
  if (_rx_op == (rfc_radioOp_t*)&_rx_adv) {
    _rx_adv.endTrigger.triggerType = TRIG_REL_START;
    _rx_adv.endTime = window_us * RAT_TICKS_PER_US;
  } else {
    _rx.endTrigger.triggerType = TRIG_REL_START;
    _rx.endTime = window_us * RAT_TICKS_PER_US;
  }

  radio_status_t st = _run_op(_tx_op);
  if (st != RADIO_OK) return st;

  // The TX status is final when the RX command starts
  uint32_t timeout = micros() + window_us + CMD_TIMEOUT_US;
  while (!_cmd_done(RF_STATUS(_rx_op))) {
    if (micros_after(timeout)) {
      _last_status = RF_STATUS(_rx_op);
      RFCDoorbellSendTo(CMDR_DIR_CMD(CMD_ABORT));
      return RADIO_ERR_TIMEOUT;
    }
  }
  _last_status = _rx_op->status;
  if (_last_status == PROP_DONE_RXTIMEOUT) return RADIO_NO_PACKET;
  if (!_cmd_ok(_last_status)) return RADIO_ERR_CMD;
  return radio_receive(pkt) ? RADIO_OK : RADIO_NO_PACKET;
}

void radio_rx_stop()
{
  if (!_powered || !_rx_running) return;
  _rx_running = false;
  if (_cmd_done(RF_STATUS(_rx_op))) return;

  RFCDoorbellSendTo(CMDR_DIR_CMD(CMD_ABORT));
  uint32_t timeout = micros() + CMD_TIMEOUT_US;
  while (!_cmd_done(RF_STATUS(_rx_op)) && micros_before(timeout)) {
  }
}

bool radio_rx_active() { return _powered && _rx_running && RF_STATUS(_rx_op) == ACTIVE; }

bool radio_receive(radio_packet_t* pkt)
{
  rfc_dataEntryGeneral_t* e = (rfc_dataEntryGeneral_t*)_rx_entries[_rx_next];
  if (ENTRY_STATUS(e) != DATA_ENTRY_FINISHED) return false;

  uint8_t* data = &e->data;
  pkt->len = _rx_len;
  memcpy(pkt->data, data, _rx_len);
  pkt->rssi = (int8_t)data[_rx_len];
  pkt->timestamp_us = micros();
  int16_t foff = _rx_foff_set[_rx_next] ? _rx_foff[_rx_next] : FOFF_NONE;
  pkt->freq_offset = foff == FOFF_NONE ? RADIO_FREQ_OFFSET_NONE
                     : (int16_t)(foff * (int32_t)_foff_deviation /
                                 (FOFF_COUNTS_PER_DEVIATION * 1000));

  // Pending first: the interrupt only sets finished entries
  ENTRY_STATUS(e) = DATA_ENTRY_PENDING;
  _rx_foff_set[_rx_next] = false;
  _rx_next = (_rx_next + 1) % RX_ENTRIES;
  return true;
}

void radio_rx_stats(radio_rx_stats_t* stats)
{
  stats->ok = _rx_out.nRxOk;
  stats->nok = _rx_out.nRxNok;
  stats->buf_full = _rx_out.nRxBufFull;
}

void radio_set_extra_overrides(const uint32_t* words, uint8_t n)
{
  if (n > RADIO_EXTRA_OVERRIDES) n = RADIO_EXTRA_OVERRIDES;
  memcpy(_extra_overrides, words, n * sizeof(uint32_t));
  _n_extra_overrides = n;
}

uint16_t radio_last_status() { return _last_status; }

int16_t radio_hposc_offset() { return _hposc ? _hposc_offset : 0; }
