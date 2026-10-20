// CYRF6936_SPI.ino of the MULTI-Module on the CC2652: a CYRF6936 behind its
// registers. The protocols write the registers, codes and TX buffer as on the
// chip; at TX GO and RX GO the data mode, framing, CRC and channel registers
// become the RF core setup and frame.c's CYRF6936 framing (1 Mchip/s, as the
// bench presets dsm and dsmbind). Read as the MPM reads them: TX_CTRL /
// RX_CTRL GO while the transaction runs, TX_IRQ_STATUS (TXC cleared on read),
// RX_IRQ_STATUS (RXC, RXE: CRC error), RX_COUNT, RX_BUFFER, RSSI.
//
// Without a length field (SDR) the packet ends as on the chip: EOP_CTRL
// successive symbols over the DATA64_THOLD chip errors. The MFG ID is the
// module id (MSB first) and 2 zero bytes: the chip's is unique per module.
//
// Not emulated: GFSK and DDR data modes (GFSK1M protocols: their own
// framings, not done yet), ACK, sleep and wake up, interrupts and GPIOs, the
// preamble count and pattern (frame.c's), TX buffer streaming beyond the
// packet length, RX buffer overflow. RX listens on the TX frequency.
//
// Without a length field the RF core receives RX_MAX bytes, longer than the
// chip, which ends at the EOP (SDR: 8.2 ms instead of 5.1 ms for the 10
// byte DSM bind response): a force end or abort while a packet comes keeps
// that RX running, an RX GO with the same setup goes on with it (the DSM
// bind check restarts RX every 7 ms).
//
// The CYRF6936 sends at TX GO: here the RF core starts about 430 us later,
// the same for every packet (DSM sends CH2 4 ms after CH1, a receiver such
// as the Lemon-RX DSMP misses packets 0.5 ms off). So nothing slow at TX GO:
// the frame_init() results are cached, the synthesizer tuned at the CHANNEL
// write, the RX only sync threshold does not set up the PHY again.
#include "iface_cyrf6936.h"

#ifdef CYRF6936_INSTALLED

#include "board.h"
#include "frame.h"
#include "radio.h"
#include "timer.h"

#define NREGS 0x40
#define BUF_SIZE 64
#define RX_MAX 16  // RX buffer of the chip

// TX_IRQ_STATUS, RX_IRQ_STATUS
#define TXC 0x02
#define TXE 0x01
#define TXB_EMPTY 0x38  // TXB15, TXB8, TXB0
#define RXC 0x02
#define RXE 0x01

// RSSI count: 21 at -60 dBm, 1.9 dB a count (datasheet)
#define RSSI_COUNT(dbm) (21 + ((dbm) + 60) * 10 / 19)

static uint8_t _reg[NREGS];
static uint8_t _sop_code[8], _data_code[16];
static uint8_t _tx_buf[BUF_SIZE], _tx_len;
static uint8_t _rx_buf[RX_MAX], _rx_count, _rx_pos;
static uint8_t _tx_irq, _rx_irq;
static uint8_t _rssi;        // RSSI register: the packet's, then live readings
static bool _rssi_pkt;       // the packet's RSSI not read yet
static enum { S_IDLE, S_TX, S_RX } _state;
static bool _rx_held;                 // RX ended for the protocol, the RF core still in it
static volatile bool _rx_busy;        // a packet's sync came, not the packet yet
static volatile uint32_t _rx_end_us;  // its expected end
static uint16_t _rx_air_max;          // fixed length RX: air bytes

static frame_t _frame;
static radio_phy_t _phy;
static bool _phy_on;
static uint32_t _freq_khz;
static uint16_t _power = RF_TX_POWER_LOW;
static radio_packet_t _pkt;
static uint8_t _air[FRAME_AIR_MAX];

// Reset values (datasheet table 6), the codes as noted there (sent LSB first)
static void _reset()
{
  static const uint8_t sop[] = {0x82, 0xC7, 0x90, 0x36, 0x21, 0x9E, 0xFF, 0x17};
  static const uint8_t data[] = {0x6F, 0xBE, 0x32, 0x01, 0xDB, 0xF1, 0x2B, 0x01,
                                 0xE3, 0x5C, 0xFA, 0x02, 0x97, 0x93, 0xF9, 0x02};
  memset(_reg, 0, sizeof(_reg));
  _reg[CYRF_00_CHANNEL] = 0x48;
  _reg[CYRF_02_TX_CTRL] = 0x03;
  _reg[CYRF_03_TX_CFG] = 0x05;
  _reg[CYRF_05_RX_CTRL] = 0x07;
  _reg[CYRF_06_RX_CFG] = 0x92;
  _reg[CYRF_0B_PWR_CTRL] = 0xA0;
  _reg[CYRF_0C_XTAL_CTRL] = 0x04;
  _reg[CYRF_0F_XACT_CFG] = 0x80;
  _reg[CYRF_10_FRAMING_CFG] = 0xA5;
  _reg[CYRF_11_DATA32_THOLD] = 0x04;
  _reg[CYRF_12_DATA64_THOLD] = 0x0A;
  _reg[CYRF_13_RSSI] = 0x20;
  _reg[CYRF_14_EOP_CTRL] = 0xA4;
  _reg[CYRF_32_AUTO_CAL_TIME] = 0x03;
  memcpy(_sop_code, sop, sizeof(sop));
  memcpy(_data_code, data, sizeof(data));
  _tx_len = _rx_count = _rx_pos = 0;
  _tx_irq = _rx_irq = 0;
  _rssi = 0;
  _rssi_pkt = false;
  _phy_on = false;  // radio_init() at the next TX or RX
}

// TX power word for TX_CFG.PA_SETTING: the MPM's high power (+4 dBm on the
// CYRF6936) at the board default, low (-18 dBm), bind and range low
static uint16_t _power_word(uint8_t pa) { return pa >= CYRF_POWER_5 ? RF_TX_POWER_DEFAULT : RF_TX_POWER_LOW; }

// frame_init() takes about 2 ms here (its RX sync word choice), too long at
// TX GO: the results for the last codes (DSM: 5 rows), CRC seed aside. The
// key holds the frame_init() inputs (newlib's byte memcmp() on frame_t: 100 us)
#define FRAME_CACHE 8
typedef struct {
  uint64_t sop, data[2];
  uint8_t flags;
} _frame_key_t;
static struct {
  _frame_key_t key;
  frame_t f;
} _frames[FRAME_CACHE];
static uint8_t _frames_n, _frames_last;

static bool _frame_init(frame_t* f)
{
  _frame_key_t k;
  memcpy(&k.sop, f->sop_code, 8);
  memcpy(k.data, f->data_code, 16);
  k.flags = f->sdr | f->data32 << 1 | f->sop << 2 | f->sop32 << 3 | f->var_len << 4 | (f->crc_len != 0) << 5 |
            f->invert << 6;
  for (uint8_t j = 0; j < _frames_n; j++) {
    uint8_t i = (_frames_last + FRAME_CACHE - j) % FRAME_CACHE;  // the latest first
    _frame_key_t* c = &_frames[i].key;
    if (c->sop == k.sop && c->data[0] == k.data[0] && c->data[1] == k.data[1] && c->flags == k.flags) {
      *f = _frames[i].f;
      return true;
    }
  }
  if (!frame_init(f)) return false;
  _frames_last = (_frames_last + 1) % FRAME_CACHE;
  _frames[_frames_last].key = k;
  _frames[_frames_last].f = *f;
  if (_frames_n < FRAME_CACHE) _frames_n++;
  return true;
}

// PHY and framing from the registers (TX or RX overrides): false if not supported
static bool _setup(radio_phy_t* phy, frame_t* f, bool tx)
{
  uint8_t tx_cfg = _reg[CYRF_03_TX_CFG], framing = _reg[CYRF_10_FRAMING_CFG];
  uint8_t mode = (tx_cfg >> 3) & 3;  // GFSK, 8DR, DDR, SDR
  if (mode != 1 && mode != 3) return false;

  memset(f, 0, sizeof(*f));
  f->type = FRAME_CYRF;
  f->sdr = mode == 3;
  f->data32 = !(tx_cfg & 0x20);
  f->sop = framing & 0x80;
  f->sop32 = !(framing & 0x40);
  f->var_len = framing & 0x20;
  memcpy(f->sop_code, _sop_code, sizeof(f->sop_code));
  memcpy(f->data_code, _data_code, sizeof(f->data_code));
  bool no_crc = tx ? _reg[CYRF_1F_TX_OVERRIDE] & 0x04 : _reg[CYRF_1E_RX_OVERRIDE] & 0x04;
  f->crc_len = no_crc ? 0 : 2;
  f->invert = tx ? _reg[CYRF_1F_TX_OVERRIDE] & 0x01 : _reg[CYRF_39_ANALOG_CTRL] & 0x02;
  if (!_frame_init(f)) return false;
  f->crc_seed = _reg[CYRF_16_CRC_SEED_MSB] << 8 | _reg[CYRF_15_CRC_SEED_LSB];

  memset(phy, 0, sizeof(*phy));  // _prepare() compares the structs
  phy->symbol_rate = 1000000;
  phy->deviation = 250000;
  phy->rx_bw = 97;
  phy->preamble_bytes = 1;  // the CYRF6936 preamble is in the frame
  phy->sync_bits = frame_sync_bits(f);
  phy->if_freq = (int16_t)0x8000;
  phy->tx_power = _power;
  phy->sync_threshold = frame_rx_sync_weak(f) ? 0x32 : 0;
  return true;
}

// CHANNEL n at 2400 + n MHz, plus TX_OFFSET (0x555: 1 MHz, the MPM's DSM
// setup: 2401 + n MHz measured)
static uint32_t _channel_khz()
{
  uint16_t offset = (_reg[CYRF_1C_TX_OFFSET_MSB] & 0x0F) << 8 | _reg[CYRF_1B_TX_OFFSET_LSB];
  return 2400000 + (_reg[CYRF_00_CHANNEL] & 0x7F) * 1000 + (offset * 1000 + 682) / 1365;
}

// Radio set up for the registers: false if not supported (nothing sent)
static bool _prepare(bool tx)
{
  radio_phy_t phy;
  frame_t f;
  if (!_setup(&phy, &f, tx)) return false;
  if (tx && _phy_on) phy.sync_threshold = _phy.sync_threshold;  // RX only: no radio_init() for it
  if (!_phy_on || memcmp(&phy, &_phy, sizeof(phy))) {
    _phy = phy;
    _freq_khz = 0;
    if (radio_init(&_phy) != RADIO_OK) {
      _phy_on = false;
      return false;
    }
    _phy_on = true;
  }
  _frame = f;
  radio_set_sync_word(frame_tx_sync(&_frame), frame_rx_sync(&_frame));
  radio_set_rx_sync_alt(frame_rx_sync_alt(&_frame));
  uint32_t khz = _channel_khz();
  if (khz != _freq_khz) {
    _freq_khz = khz;
    radio_set_frequency(khz);
  }
  return true;
}

// Idle: the synthesizer to the new channel now, not at TX GO (DSM writes
// CH1's channel 600 us ahead, CH2's 2 ms)
static void _tune()
{
  uint32_t khz = _channel_khz();
  if (_phy_on && khz != _freq_khz) {
    _freq_khz = khz;
    radio_set_frequency(khz);
  }
}

// End of the transaction (force end, abort): a packet being sent goes out
static void _stop()
{
  if (_state == S_RX) {
    if (_rx_busy && (int32_t)(_rx_end_us - micros()) > 0) _rx_held = true;
    else radio_rx_stop();
  }
  _state = S_IDLE;
}

// Ends the held RX, unless its setup is the same as the next RX's
static bool _release(const radio_phy_t* phy, const frame_t* f)
{
  if (!_rx_held) return false;
  _rx_held = false;
  bool same = phy && !memcmp(phy, &_phy, sizeof(*phy)) && !memcmp(f, &_frame, sizeof(*f)) &&
              _channel_khz() == _freq_khz;
  if (!same) {
    radio_rx_stop();
    _rx_busy = false;
  }
  return same;
}

static void _tx()
{
  _stop();
  _release(NULL, NULL);
  uint16_t power = _power_word(_reg[CYRF_03_TX_CFG] & 7);
  if (power != _power) {
    _power = _phy.tx_power = power;
    if (_phy_on) radio_set_tx_power(power);
  }
  uint8_t len = _reg[CYRF_01_TX_LENGTH];
  uint16_t n = 0;
  if (len <= _tx_len && _prepare(true)) n = frame_tx_data(&_frame, _tx_buf, len, _air);
  _tx_len = 0;
  if (!n) {
    _tx_irq |= TXE;
    return;
  }
  radio_tx_start(_air, n);
  _state = S_TX;
}

// Packet length after its first bytes (fixed length: RX_MAX), its expected end
static uint16_t _rx_var_len(const uint8_t* data, uint16_t n, void* ctx)
{
  (void)ctx;
  uint16_t len = _frame.var_len ? frame_rx_var_len(&_frame, RX_MAX, data, n) : _rx_air_max;
  if (len > n) {
    _rx_end_us = micros() + (uint32_t)(len - n) * 8000000 / _phy.symbol_rate;
    _rx_busy = true;
  }
  return len;
}

static void _rx()
{
  _stop();
  _rx_irq = 0;
  _rx_count = _rx_pos = 0;
  if (_rx_held) {
    radio_phy_t phy;
    frame_t f;
    bool ok = _setup(&phy, &f, false);
    if (_release(ok ? &phy : NULL, &f)) {
      _state = S_RX;  // the packet being received
      return;
    }
  }
  _rx_busy = false;
  if (!_prepare(false)) return;
  uint8_t hdr_len = frame_rx_hdr_len(&_frame);
  _rx_air_max = frame_rx_len(&_frame, RX_MAX);
  radio_status_t st = radio_rx_start_var(hdr_len ? hdr_len : 1, _rx_air_max, _rx_var_len, 0);
  if (st == RADIO_OK) _state = S_RX;
}

static uint8_t _rssi_count(int8_t dbm)
{
  int16_t c = RSSI_COUNT(dbm);
  return c < 0 ? 0 : c > 31 ? 31 : (uint8_t)c;
}

// End of TX, packet received: the transaction ends (RX: one packet)
static void _poll()
{
  if (_state == S_TX && radio_tx_done()) {
    _state = S_IDLE;
    _tx_irq |= TXC;
  }
  while (_state == S_RX && radio_receive(&_pkt)) {
    _rx_busy = false;
    uint8_t payload[FRAME_PAYLOAD_MAX];
    uint8_t n = 0, len = RX_MAX;
    if (!_frame.var_len) {
      // EOP: successive symbols with no data code
      uint8_t th = _frame.data32 ? _reg[CYRF_11_DATA32_THOLD] & 0x0F : _reg[CYRF_12_DATA64_THOLD] & 0x1F;
      len = frame_cyrf_rx_count(&_frame, _pkt.data, _pkt.len, th, _reg[CYRF_14_EOP_CTRL] & 0x0F);
      if (len > RX_MAX) len = RX_MAX;
    }
    frame_result_t r = len ? frame_decode(&_frame, _pkt.data, _pkt.len, len, payload, &n) : FRAME_SHORT;
    if (r == FRAME_SHORT || n == 0) continue;  // false sync: still in RX
    radio_rx_stop();
    _state = S_IDLE;
    memcpy(_rx_buf, payload, n);
    _rx_count = n;
    _rx_pos = 0;
    _reg[CYRF_0A_RX_LENGTH] = _frame.var_len ? n : 0;
    _reg[CYRF_08_RX_STATUS] = r == FRAME_OK ? 0 : 0x08;  // Bad CRC
    _rx_irq = RXC | (r == FRAME_OK ? 0 : RXE);
    _rssi = _rssi_count(_pkt.rssi);
    _rssi_pkt = true;
  }
}

void CYRF_WriteRegister(uint8_t address, uint8_t data)
{
  address &= 0x3F;
  switch (address) {
    case CYRF_00_CHANNEL:
      _reg[address] = data & 0x7F;
      if (_state == S_RX) _rx();  // RSSI scans: RX on the new channel
      else if (_state == S_IDLE) _tune();
      return;
    case CYRF_02_TX_CTRL:
      _reg[address] = data & 0x3F;
      if (data & 0x40) _tx_len = 0;  // TX CLR
      if (data & 0x80) _tx();        // TX GO
      return;
    case CYRF_05_RX_CTRL:
      _reg[address] = data & 0x3F;
      if (data & 0x80) _rx();  // RX GO
      return;
    case CYRF_0F_XACT_CFG:
      _reg[address] = data & ~0x20;
      if (data & 0x20) _stop();  // FRC END
      return;
    case CYRF_1D_MODE_OVERRIDE:
      if (data & 0x01) {  // RST
        _stop();
        _release(NULL, NULL);
        _reset();
      }
      return;
    case CYRF_20_TX_BUFFER:
      if (_tx_len < BUF_SIZE) _tx_buf[_tx_len++] = data;
      return;
    case CYRF_04_TX_IRQ_STATUS: case CYRF_07_RX_IRQ_STATUS: case CYRF_08_RX_STATUS:
    case CYRF_09_RX_COUNT: case CYRF_0A_RX_LENGTH: case CYRF_13_RSSI: case CYRF_21_RX_BUFFER:
      return;  // read only (RX_IRQ_STATUS.RXOW: no overwrite here)
    default:
      _reg[address] = data;
  }
}

static void CYRF_WriteRegisterMulti(uint8_t address, const uint8_t data[], uint8_t length)
{
  address &= 0x3F;
  if (address == CYRF_22_SOP_CODE || address == CYRF_23_DATA_CODE) {
    uint8_t* code = address == CYRF_22_SOP_CODE ? _sop_code : _data_code;
    uint8_t size = address == CYRF_22_SOP_CODE ? sizeof(_sop_code) : sizeof(_data_code);
    memcpy(code, data, length < size ? length : size);
    return;
  }
  // No address increment: the files fill up, a register takes each byte
  for (uint8_t i = 0; i < length; i++) CYRF_WriteRegister(address, data[i]);
}

static uint8_t _read(uint8_t address, uint8_t i)
{
  switch (address) {
    case CYRF_02_TX_CTRL:
      _poll();
      return _reg[address] | (_state == S_TX ? 0x80 : 0);
    case CYRF_04_TX_IRQ_STATUS: {
      _poll();
      uint8_t v = _tx_irq | (_tx_len ? 0 : TXB_EMPTY);
      _tx_irq = 0;  // TXC, TXE cleared on read
      return v;
    }
    case CYRF_05_RX_CTRL:
      _poll();
      return _reg[address] | (_state == S_RX ? 0x80 : 0);
    case CYRF_07_RX_IRQ_STATUS: {
      _poll();
      uint8_t left = _rx_count - _rx_pos;
      return _rx_irq | (left >= 16 ? 0x20 : 0) | (left >= 8 ? 0x10 : 0) | (left ? 0x08 : 0);
    }
    case CYRF_09_RX_COUNT:
      return _rx_count;
    case CYRF_13_RSSI: {
      // The packet's (SOP), then a new reading at each read
      uint8_t lna = _reg[CYRF_06_RX_CFG] & 0x40 ? 0x20 : 0;
      if (_rssi_pkt) {
        _rssi_pkt = false;
        return 0x80 | lna | _rssi;
      }
      if (_state == S_RX) {
        int8_t dbm = radio_rssi();
        if (dbm != RADIO_RSSI_NONE) _rssi = _rssi_count(dbm);
      }
      return lna | _rssi;
    }
    case CYRF_21_RX_BUFFER:
      return _rx_pos < _rx_count ? _rx_buf[_rx_pos++] : 0;
    case CYRF_22_SOP_CODE:
      return _sop_code[i % sizeof(_sop_code)];
    case CYRF_23_DATA_CODE:
      return _data_code[i % sizeof(_data_code)];
    case CYRF_25_MFG_ID: {
      uint32_t id = MProtocol_id_master;
      return i < 4 ? id >> (24 - 8 * i) : 0;
    }
    default:
      return _reg[address];
  }
}

static void CYRF_ReadRegisterMulti(uint8_t address, uint8_t data[], uint8_t length)
{
  for (uint8_t i = 0; i < length; i++) data[i] = _read(address & 0x3F, i);
}

uint8_t CYRF_ReadRegister(uint8_t address) { return _read(address & 0x3F, 0); }

// Writes the SPI bus directly upstream: count, then the 2 pattern bytes
void CYRF_WritePreamble(uint32_t preamble)
{
  (void)preamble;  // frame.c's preamble
}

// mpm-v2: CYRF6936_SPI.ino from here, verbatim but CYRF_WritePreamble() and
// CYRF_WriteDataPacketLen() (not static: DSM)

uint8_t CYRF_Reset()
{
#ifdef CYRF_RST_HI
	CYRF_RST_HI;										//Hardware reset
	delayMicroseconds(100);
	CYRF_RST_LO;
	delayMicroseconds(100);		  
#endif
	CYRF_WriteRegister(CYRF_1D_MODE_OVERRIDE, 0x01);	//Software reset
	delayMicroseconds(200);
	CYRF_WriteRegister(CYRF_0C_XTAL_CTRL, 0xC0);		//Enable XOUT as GPIO
	CYRF_WriteRegister(CYRF_0D_IO_CFG, 0x04);			//Enable PACTL as GPIO
	CYRF_SetTxRxMode(TXRX_OFF);
	//Verify the CYRF chip is responding
	return (CYRF_ReadRegister(CYRF_10_FRAMING_CFG) == 0xa5);
}

/*
*
*/
void CYRF_GetMfgData(uint8_t data[])
{
#ifndef FORCE_CYRF_ID
	if(eeprom_read_byte((EE_ADDR)EEPROM_CID_INIT_OFFSET)==0xf0)
	{//read Cyrf ID from EEPROM
		for(uint8_t i=0;i<6;i++)
			data[i] = eeprom_read_byte((EE_ADDR)EEPROM_CID_OFFSET+i);
	}
	else
	{//read Cyrf ID and store it EEPROM
		/* Fuses power on */
		CYRF_WriteRegister(CYRF_25_MFG_ID, 0xFF);

		CYRF_ReadRegisterMulti(CYRF_25_MFG_ID, data, 6);
		for(uint8_t i=0;i<6;i++)
			eeprom_write_byte((EE_ADDR)EEPROM_CID_OFFSET+i, data[i]);
		eeprom_write_byte((EE_ADDR)EEPROM_CID_INIT_OFFSET, 0xf0);

		/* Fuses power off */
		CYRF_WriteRegister(CYRF_25_MFG_ID, 0x00);
	}
#else
	memcpy(data,FORCE_CYRF_ID,6);
#endif
}

/*
* 1 - Tx else Rx
*/
void CYRF_SetTxRxMode(uint8_t mode)
{
	if(mode==TXRX_OFF)
	{
		if( protocol!=PROTO_WFLY && protocol!=PROTO_MLINK )
			CYRF_WriteRegister(CYRF_0F_XACT_CFG, 0x24); // 4=IDLE, 8=TX, C=RX
		CYRF_WriteRegister(CYRF_0E_GPIO_CTRL,0x00); // XOUT=0 PACTL=0
	}
	else
	{
		//Set the post tx/rx state
		if( protocol!=PROTO_WFLY && protocol!=PROTO_MLINK )
			CYRF_WriteRegister(CYRF_0F_XACT_CFG, mode == TX_EN ? 0x28 : 0x2C); // 4=IDLE, 8=TX, C=RX
		if(mode == TX_EN)
#ifdef ORANGE_TX_BLUE
			CYRF_WriteRegister(CYRF_0E_GPIO_CTRL,0x20); // XOUT=1, PACTL=0
		else
			CYRF_WriteRegister(CYRF_0E_GPIO_CTRL,0x80);	// XOUT=0, PACTL=1
#else
			CYRF_WriteRegister(CYRF_0E_GPIO_CTRL,0x80); // XOUT=1, PACTL=0
		else
			CYRF_WriteRegister(CYRF_0E_GPIO_CTRL,0x20);	// XOUT=0, PACTL=1
#endif
	}
}
/*
*
*/
void CYRF_ConfigRFChannel(uint8_t ch)
{
	CYRF_WriteRegister(CYRF_00_CHANNEL,ch);
}

/*
static void CYRF_SetPower_Value(uint8_t power)
{
	uint8_t val = CYRF_ReadRegister(CYRF_03_TX_CFG) & 0xF8;
	CYRF_WriteRegister(CYRF_03_TX_CFG, val | (power & 0x07));
}
*/

void CYRF_SetPower(uint8_t val)
{
	uint8_t power=CYRF_BIND_POWER;
	if(IS_BIND_DONE)
		#ifdef CYRF6936_ENABLE_LOW_POWER
			power=IS_POWER_FLAG_on?CYRF_HIGH_POWER:CYRF_LOW_POWER;
		#else
			power=CYRF_HIGH_POWER;
		#endif
	if(IS_RANGE_FLAG_on)
		power=CYRF_RANGE_POWER;
	power|=val;
	if(prev_power != power)
	{
		CYRF_WriteRegister(CYRF_03_TX_CFG,power);
		prev_power=power;
	}

	#ifdef USE_CYRF6936_CH15_TUNING
		static uint16_t Channel15=1024;
		if(Channel15!=Channel_data[CH15])
		{ // adjust frequency
			Channel15=Channel_data[CH15]+0x155;	// default value is 0x555 = 0x400 + 0x155
			CYRF_WriteRegister(CYRF_1B_TX_OFFSET_LSB, Channel15&0xFF);
			CYRF_WriteRegister(CYRF_1C_TX_OFFSET_MSB, Channel15>>8);
			Channel15-=0x155;
		}
	#endif
}

/*
*
*/
void CYRF_ConfigCRCSeed(uint16_t crc_seed)
{
	CYRF_WriteRegister(CYRF_15_CRC_SEED_LSB,crc_seed & 0xff);
	CYRF_WriteRegister(CYRF_16_CRC_SEED_MSB,crc_seed >> 8);
}
/*
* these are the recommended sop codes from Cyrpress
* See "WirelessUSB LP/LPstar and PRoC LP/LPstar Technical Reference Manual"
*/
void CYRF_ConfigSOPCode(const uint8_t *sopcodes)
{
	//NOTE: This can also be implemented as:
	//for(i = 0; i < 8; i++) WriteRegister)0x23, sopcodes[i];
	CYRF_WriteRegisterMulti(CYRF_22_SOP_CODE, sopcodes, 8);
}

void CYRF_ConfigDataCode(const uint8_t *datacodes)
{
	//NOTE: This can also be implemented as:
	//for(i = 0; i < 16; i++) WriteRegister)0x23, datacodes[i];
	CYRF_WriteRegisterMulti(CYRF_23_DATA_CODE, datacodes, 16);
}

/*
*
*/
/*static void CYRF_ReadDataPacket(uint8_t dpbuffer[])
{
	CYRF_ReadRegisterMulti(CYRF_21_RX_BUFFER, dpbuffer, 0x10);
}
*/
void CYRF_ReadDataPacketLen(uint8_t dpbuffer[], uint8_t length)
{
    CYRF_ReadRegisterMulti(CYRF_21_RX_BUFFER, dpbuffer, length);
}

void CYRF_WriteDataPacketLen(  // mpm-v2: DSM, other file
	const uint8_t dpbuffer[], uint8_t len)
{
	CYRF_WriteRegister(CYRF_01_TX_LENGTH, len);
	CYRF_WriteRegister(CYRF_02_TX_CTRL, 0x43);	// 0x40
	CYRF_WriteRegisterMulti(CYRF_20_TX_BUFFER, dpbuffer, len);
	CYRF_WriteRegister(CYRF_02_TX_CTRL, 0x83);	// 0xBF
}

void CYRF_WriteDataPacket(const uint8_t dpbuffer[])
{
	CYRF_WriteDataPacketLen(dpbuffer, 16);
}

/*static uint8_t CYRF_ReadRSSI(uint8_t dodummyread)
{
	uint8_t result;
	if(dodummyread)
		CYRF_ReadRegister(CYRF_13_RSSI);
	result = CYRF_ReadRegister(CYRF_13_RSSI);
	if(result & 0x80)
		result = CYRF_ReadRegister(CYRF_13_RSSI);
	return (result & 0x0F);
}
*/
//NOTE: This routine will reset the CRC Seed
void CYRF_FindBestChannels(uint8_t *channels, uint8_t len, uint8_t minspace, uint8_t min, uint8_t max, uint8_t forced)
{
	#define NUM_FREQ 80
	#define FREQ_OFFSET 4
	uint8_t rssi[NUM_FREQ];

	if (min < FREQ_OFFSET)
		min = FREQ_OFFSET;
	if (max > NUM_FREQ)
		max = NUM_FREQ;

	uint8_t i;
	int8_t j;
	memset(channels, 0, sizeof(uint8_t) * len);
	CYRF_ConfigCRCSeed(0x0000);
	CYRF_SetTxRxMode(RX_EN);
	//Wait for pre-amp to switch from send to receive
	delayMilliseconds(1);
	for(i = 0; i < NUM_FREQ; i++)
	{
		if(((i&1) && forced == FIND_CHANNEL_EVEN) || (!(i&1) && forced == FIND_CHANNEL_ODD))
		{
			rssi[i] = 0xFF;
			continue;
		}
		CYRF_ConfigRFChannel(i);	//protocol==PROTO_LOSI?i|1:i);
		delayMicroseconds(270);					//slow channel require 270usec for synthesizer to settle
        if( !(CYRF_ReadRegister(CYRF_05_RX_CTRL) & 0x80)) {
            CYRF_WriteRegister(CYRF_05_RX_CTRL, 0x80); //Prepare to receive
            delayMicroseconds(15);
            CYRF_ReadRegister(CYRF_13_RSSI);	//dummy read
            delayMicroseconds(15);				//The conversion can occur as often as once every 12us
        }
		rssi[i] = CYRF_ReadRegister(CYRF_13_RSSI)&0x1F;
	}

	for (i = 0; i < len; i++)
	{
		channels[i] = min;
		for (j = min; j < max; j++)
			if (rssi[j] < rssi[channels[i]])
				channels[i] = j;
		for (j = channels[i] - minspace; j < channels[i] + minspace; j++) {
			//Ensure we don't reuse any channels within minspace of the selected channel again
			if (j < 0 || j >= NUM_FREQ)
				continue;
			rssi[j] = 0xff;
		}
	}
	CYRF_WriteRegister(CYRF_29_RX_ABORT, 0x20);		// Abort RX operation
	CYRF_SetTxRxMode(TX_EN);
	CYRF_WriteRegister(CYRF_29_RX_ABORT, 0x00);		// Clear abort RX
}

#if defined(DEVO_CYRF6936_INO) || defined(J6PRO_CYRF6936_INO) || defined(TRAXXAS_CYRF6936_INO)
const uint8_t PROGMEM DEVO_j6pro_sopcodes[][8] = {
    /* Note these are in order transmitted (LSB 1st) */
    {0x3C, 0x37, 0xCC, 0x91, 0xE2, 0xF8, 0xCC, 0x91},
    {0x9B, 0xC5, 0xA1, 0x0F, 0xAD, 0x39, 0xA2, 0x0F},
    {0xEF, 0x64, 0xB0, 0x2A, 0xD2, 0x8F, 0xB1, 0x2A},
    {0x66, 0xCD, 0x7C, 0x50, 0xDD, 0x26, 0x7C, 0x50},
    {0x5C, 0xE1, 0xF6, 0x44, 0xAD, 0x16, 0xF6, 0x44},
    {0x5A, 0xCC, 0xAE, 0x46, 0xB6, 0x31, 0xAE, 0x46},
    {0xA1, 0x78, 0xDC, 0x3C, 0x9E, 0x82, 0xDC, 0x3C},
    {0xB9, 0x8E, 0x19, 0x74, 0x6F, 0x65, 0x18, 0x74},
    {0xDF, 0xB1, 0xC0, 0x49, 0x62, 0xDF, 0xC1, 0x49},
    {0x97, 0xE5, 0x14, 0x72, 0x7F, 0x1A, 0x14, 0x72},
#if defined(J6PRO_CYRF6936_INO) || defined(TRAXXAS_CYRF6936_INO)
    {0x82, 0xC7, 0x90, 0x36, 0x21, 0x03, 0xFF, 0x17},
    {0xE2, 0xF8, 0xCC, 0x91, 0x3C, 0x37, 0xCC, 0x91}, //Note: the '03' was '9E' in the Cypress recommended table
    {0xAD, 0x39, 0xA2, 0x0F, 0x9B, 0xC5, 0xA1, 0x0F}, //The following are the same as the 1st 8 above,
    {0xD2, 0x8F, 0xB1, 0x2A, 0xEF, 0x64, 0xB0, 0x2A}, //but with the upper and lower word swapped
    {0xDD, 0x26, 0x7C, 0x50, 0x66, 0xCD, 0x7C, 0x50},
    {0xAD, 0x16, 0xF6, 0x44, 0x5C, 0xE1, 0xF6, 0x44},
    {0xB6, 0x31, 0xAE, 0x46, 0x5A, 0xCC, 0xAE, 0x46},
    {0x9E, 0x82, 0xDC, 0x3C, 0xA1, 0x78, 0xDC, 0x3C},
    {0x6F, 0x65, 0x18, 0x74, 0xB9, 0x8E, 0x19, 0x74},
    {0x62, 0xDF, 0xC1, 0x49, 0xDF, 0xB1, 0xC0, 0x49}, //j6pro bind
#endif
};
#endif

static void __attribute__((unused)) CYRF_PROGMEM_ConfigSOPCode(const uint8_t *data)
{
	uint8_t code[8];
	//debug("SOP:");
	for(uint8_t i=0;i<8;i++)
	{
		code[i]=pgm_read_byte_near(&data[i]);
		//debug(" %02X",code[i]);
	}
	//debugln("");
	CYRF_ConfigSOPCode(code);
}

//CYRF GFSK 1Mb functions
const uint8_t PROGMEM CYRF_GFSK1M_init_vals[][2] = {
	{CYRF_02_TX_CTRL, 0x00},		// transmit err & complete interrupts disabled
	{CYRF_05_RX_CTRL, 0x00},		// receive err & complete interrupts disabled
	{CYRF_28_CLK_EN, 0x02},			// Force Receive Clock Enable, MUST be set
	{CYRF_32_AUTO_CAL_TIME, 0x3c},	// must be set to 3C
	{CYRF_35_AUTOCAL_OFFSET, 0x14},	// must be  set to 14
	{CYRF_06_RX_CFG, 0x48},			// LNA manual control, Rx Fast Turn Mode Enable
	{CYRF_1B_TX_OFFSET_LSB, 0x00},	// Tx frequency offset LSB
	{CYRF_1C_TX_OFFSET_MSB, 0x00},	// Tx frequency offset MSB
	{CYRF_0F_XACT_CFG, 0x24},		// Force End State, transaction end state = idle
	{CYRF_03_TX_CFG, 0x00},			// GFSK mode
	{CYRF_12_DATA64_THOLD, 0x0a},	// 64 Chip Data PN Code Correlator Threshold = 10
	{CYRF_0F_XACT_CFG, 0x04},		// Transaction End State = idle
	{CYRF_39_ANALOG_CTRL, 0x01},	// synth setting time for all channels is the same as for slow channels
	{CYRF_0F_XACT_CFG, 0x24},		//Force IDLE
	{CYRF_29_RX_ABORT, 0x00},		//Clear RX abort
	{CYRF_12_DATA64_THOLD, 0x0a},	//set pn correlation threshold
	{CYRF_10_FRAMING_CFG, 0x4a},	//set sop len and threshold
	{CYRF_29_RX_ABORT, 0x0f},		//Clear RX abort?
	{CYRF_03_TX_CFG, 0x00},			// GFSK mode
	{CYRF_10_FRAMING_CFG, 0x4a},	// 0b11000000 //set sop len and threshold
	{CYRF_1F_TX_OVERRIDE, 0x04},	//disable tx CRC
	{CYRF_1E_RX_OVERRIDE, 0x14},	//disable rx crc
	{CYRF_14_EOP_CTRL, 0x00},		//set EOP sync == 0
};
static void __attribute__((unused)) CYRF_GFSK1M_Init(uint8_t payload_length, uint8_t preamble_len)
{
	for(uint8_t i = 0; i < sizeof(CYRF_GFSK1M_init_vals) / 2; i++)	
		CYRF_WriteRegister(pgm_read_byte_near(&CYRF_GFSK1M_init_vals[i][0]), pgm_read_byte_near(&CYRF_GFSK1M_init_vals[i][1]));


	CYRF_WriteRegister(CYRF_01_TX_LENGTH, payload_length);
	
	CYRF_WritePreamble(0xAAAA00 | preamble_len);

	CYRF_SetPower(0x00);
	
	CYRF_SetTxRxMode(TX_EN);
}
static void __attribute__((unused)) CYRF_GFSK1M_SendPayload(uint8_t *buffer, uint8_t len)
{
	uint8_t send=len>16 ? 16 : len;
	CYRF_WriteRegister(CYRF_02_TX_CTRL, 0x40);
	CYRF_WriteRegisterMulti(CYRF_20_TX_BUFFER, buffer, send);			// Fill the buffer with 16 bytes max
	CYRF_WriteRegister(CYRF_02_TX_CTRL, 0x80);							// Start send
	buffer += send;
	len -= send;

	while(len>8)
	{
		while((CYRF_ReadRegister(CYRF_04_TX_IRQ_STATUS)&0x10) == 0);	// Wait that half of the buffer is empty
		CYRF_WriteRegisterMulti(CYRF_20_TX_BUFFER, buffer, 8);			// Add 8 bytes to the buffer
		buffer+=8;
		len-=8;
	}

	if(len)
	{
		while((CYRF_ReadRegister(CYRF_04_TX_IRQ_STATUS)&0x10) == 0);	// Wait that half of the buffer is empty
		CYRF_WriteRegisterMulti(CYRF_20_TX_BUFFER, buffer, len);		// Add the remaining bytes to the buffer
	}
}
#define CYRF_GFSK1M_SetPower() CYRF_SetPower(0x00)
#endif