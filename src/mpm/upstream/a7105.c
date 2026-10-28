// A7105_SPI.ino of the MULTI-Module on the CC2652: an A7105 behind its
// registers. The protocols write the registers and the FIFO and strobe as on
// the chip; at TX and RX the data rate, code (ID length, CRC, FEC, preamble),
// FIFO end and PLL registers become the RF core setup and frame.c's A7105
// framing. MODE reads as on the chip after RX: TRER cleared once a packet is
// in, CRCF set if its CRC failed. RSSI_THOLD reads the packet's RSSI.
//
// Not emulated: whitening (WHTS), segment FIFOs, the A7105 deviation
// (always 250 kHz: the A7105 receives it best, see the bench presets),
// calibration (done at once, VCO_SBCAL_I reads back what was written)
#include "iface_a7105.h"

#include "board.h"
#include "frame.h"
#include "radio.h"

#define NREGS 0x33
#define FIFO_SIZE 64
#define FPFD 32000000ULL  // 16 MHz crystal, doubled
#define DEVIATION 250000

// MODE register (read)
#define MODE_TRER 0x01  // TX or RX on
#define MODE_TRSR 0x02  // TX (with TRER)
#define MODE_ON 0x1C    // PLL, crystal, chip enabled
#define MODE_CRCF 0x20
#define MODE_FECF 0x40

static uint8_t _reg[NREGS];
static uint8_t _id[4];
static uint8_t _tx_fifo[FIFO_SIZE];
static uint8_t _rx_fifo[FIFO_SIZE];
static enum { S_STANDBY, S_RX, S_TX } _state;
static uint8_t _flags;     // CRCF, FECF of the last packet
static uint8_t _rssi_adc;  // RSSI_THOLD read value

static frame_t _frame;
static radio_phy_t _phy;
static bool _phy_on;
static uint32_t _freq_khz;
static uint16_t _power;
static radio_packet_t _pkt;
static uint8_t _air[FRAME_AIR_MAX];

static void _reset()
{
  memset(_reg, 0, sizeof(_reg));
  _reg[A7105_03_FIFOI] = 0x3F;
  _reg[A7105_10_PLL_II] = 0x9E;
  _reg[A7105_11_PLL_III] = 0x4B;
  _reg[A7105_13_PLL_V] = 0x02;
  _reg[A7105_1F_CODE_I] = 0x07;
  memset(_id, 0, sizeof(_id));
}

// TX power word for TX_TEST (PAC, TBG): the MPM's high power (+1 dBm on the
// A7105) at the board default, low power (-11 dBm) reduced, bind and range
// (-23 dBm) low
static uint16_t _power_word(uint8_t tx_test)
{
  if (tx_test >= A7105_POWER_4) return RF_TX_POWER_DEFAULT;
  return tx_test >= A7105_POWER_3 ? RF_TX_POWER_REDUCED : RF_TX_POWER_LOW;
}

static uint8_t _len() { return _reg[A7105_03_FIFOI] + 1 > FIFO_SIZE ? FIFO_SIZE : _reg[A7105_03_FIFOI] + 1; }

// PHY and framing from the registers: false if not supported
static bool _setup(radio_phy_t* phy, frame_t* f)
{
  uint8_t code = _reg[A7105_1F_CODE_I];
  if (code & 0x20) return false;  // whitening

  memset(f, 0, sizeof(*f));
  f->type = FRAME_A7105;
  memcpy(f->addr, _id, 4);
  f->addr_len = code & 0x04 ? 4 : 2;
  f->crc_len = code & 0x08 ? 2 : 0;
  f->fec = code & 0x10;
  if (!frame_init(f)) return false;

  uint32_t rate = 500000 / (_reg[A7105_0E_DATA_RATE] + 1);
  memset(phy, 0, sizeof(*phy));  // _prepare() compares the structs
  phy->symbol_rate = rate;
  phy->deviation = DEVIATION;
  phy->rx_bw = rate > 250000 ? 97 : 94;  // as the bench presets
  phy->preamble_bytes = (code & 3) + 1;
  phy->sync_bits = frame_sync_bits(f);
  phy->if_freq = (int16_t)0x8000;
  phy->tx_power = _power;
  phy->sync_threshold = frame_rx_sync_weak(f) ? 0x32 : 0;
  return true;
}

// LO base (BIP, BFP) + channel x step (CHR)
static uint32_t _channel_khz()
{
  uint16_t bfp = _reg[A7105_12_PLL_IV] << 8 | _reg[A7105_13_PLL_V];
  uint64_t hz = _reg[A7105_11_PLL_III] * FPFD + ((bfp * FPFD) >> 16);
  uint8_t chr = (_reg[A7105_10_PLL_II] >> 1) & 0x0F;
  hz += _reg[A7105_0F_PLL_I] * (FPFD / 4 / (chr + 1));
  return (uint32_t)((hz + 500) / 1000);
}

static bool _prepare()
{
  radio_phy_t phy;
  frame_t f;
  if (!_setup(&phy, &f)) return false;
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
  uint32_t khz = _channel_khz();
  if (khz != _freq_khz) {
    _freq_khz = khz;
    radio_set_frequency(khz);
  }
  return true;
}

static void _standby()
{
  if (_state == S_RX) radio_rx_stop();
  _state = S_STANDBY;
}

static void _tx()
{
  _standby();
  if (!_prepare()) return;
  uint16_t n = frame_tx_data(&_frame, _tx_fifo, _len(), _air);
  if (!n) return;
  radio_tx_start(_air, n);
  _state = S_TX;
}

static void _rx()
{
  _standby();
  _flags = 0;
  if (!_prepare()) return;
  if (radio_rx_start(frame_rx_len(&_frame, _len())) == RADIO_OK) _state = S_RX;
}

// A7105 RSSI (ADC): 8 at -50 dBm and above to 160 at -105 dBm (datasheet
// range, fig. 17.1)
static uint8_t _adc(int8_t dbm)
{
  int16_t v = 8 + (-50 - dbm) * 152 / 55;
  return v < 8 ? 8 : v > 160 ? 160 : v;
}

// End of TX, packet received: back to standby, as the chip
static void _poll()
{
  if (_state == S_TX && radio_tx_done()) _state = S_STANDBY;
  if (_state != S_RX || !radio_receive(&_pkt)) return;
  uint8_t n = 0;
  frame_result_t r = frame_decode(&_frame, _pkt.data, _pkt.len, _len(), _rx_fifo, &n);
  _flags = r == FRAME_OK ? 0 : MODE_CRCF;
  _rssi_adc = _adc(_pkt.rssi);
  radio_rx_stop();
  _state = S_STANDBY;
}

void A7105_WriteData(uint8_t len, uint8_t channel)
{
	memcpy(_tx_fifo, packet, len < FIFO_SIZE ? len : FIFO_SIZE);  // mpm-v2: the FIFO write
	if(protocol!=PROTO_WFLY2)
	{
		if(!(protocol==PROTO_FLYSKY || (protocol==PROTO_KYOSHO && sub_protocol==KYOSHO_HYPE)))
		{
			A7105_Strobe(A7105_STANDBY);	//Force standby mode, ie cancel any TX or RX...
			A7105_SetTxRxMode(TX_EN);		//Switch to PA
		}
		A7105_WriteReg(A7105_0F_PLL_I, channel);
		A7105_Strobe(A7105_TX);
	}
}

void A7105_ReadData(uint8_t len)
{
  _poll();
  memcpy(packet, _rx_fifo, len < FIFO_SIZE ? len : FIFO_SIZE);
}

void A7105_WriteReg(uint8_t address, uint8_t data)
{
  if (address == A7105_00_MODE) {
    if (data == 0x00) A7105_Reset();
    return;
  }
  if (address >= NREGS) return;
  _reg[address] = data;
  if (address == A7105_28_TX_TEST) {
    uint16_t power = _power_word(data);
    if (power != _power) {
      _power = _phy.tx_power = power;
      if (_phy_on) radio_set_tx_power(power);
    }
  }
}

uint8_t A7105_ReadReg(uint8_t address)
{
  address &= 0x3F;
  _poll();
  switch (address) {
    case A7105_00_MODE:
      return MODE_ON | _flags | (_state == S_TX ? MODE_TRER | MODE_TRSR : 0) |
             (_state == S_RX ? MODE_TRER : 0);
    case A7105_02_CALC: return 0;  // calibration done
    case A7105_1D_RSSI_THOLD: {
      if (_state != S_RX) return _rssi_adc;
      int8_t dbm = radio_rssi();
      return dbm == RADIO_RSSI_NONE ? _rssi_adc : _adc(dbm);
    }
    default: return address < NREGS ? _reg[address] : 0;
  }
}

// The GPIO pins switch the 4-in-1's PA and LNA: the RF core does it here
void A7105_SetTxRxMode(uint8_t mode) { (void)mode; }

uint8_t A7105_Reset()
{
  _standby();
  _reset();
  _flags = 0;
  _rssi_adc = 160;
  _power = RF_TX_POWER_LOW;
  _phy_on = false;  // radio_init() at the next TX or RX
  return 1;
}

void A7105_WriteID(uint32_t ida)
{
  _id[0] = ida >> 24;
  _id[1] = ida >> 16;
  _id[2] = ida >> 8;
  _id[3] = ida;
}

void A7105_Strobe(uint8_t address)
{
  switch (address) {
    case A7105_TX: _tx(); break;
    case A7105_RX: _rx(); break;
    case A7105_SLEEP:
    case A7105_IDLE:
    case A7105_STANDBY:
    case A7105_PLL: _standby(); break;
    default: break;  // FIFO pointer resets
  }
}

//
// A7105_SPI.ino, unchanged
//

void A7105_SetPower()
{
	uint8_t power=A7105_BIND_POWER;
	if(IS_BIND_DONE)
		#ifdef A7105_ENABLE_LOW_POWER
			power=IS_POWER_FLAG_on?A7105_HIGH_POWER:A7105_LOW_POWER;
		#else
			power=A7105_HIGH_POWER;
		#endif
	if(IS_RANGE_FLAG_on)
		power=A7105_RANGE_POWER;
	if(prev_power != power)
	{
		A7105_WriteReg(A7105_28_TX_TEST, power);
		prev_power=power;
	}
}


void A7105_AdjustLOBaseFreq(uint8_t cmd)
{
	static int16_t old_offset=2048;
	int16_t offset=1024;
	if(cmd==0)
	{	// Called at init of the A7105
		old_offset=2048;
		switch(protocol)
		{
			case PROTO_HUBSAN:
				#ifdef FORCE_HUBSAN_TUNING
					offset=(int16_t)FORCE_HUBSAN_TUNING;
				#endif
				break;
			case PROTO_BUGS:
				#ifdef FORCE_BUGS_TUNING
					offset=(int16_t)FORCE_BUGS_TUNING;
				#endif
				break;
			case PROTO_FLYSKY:
				#ifdef FORCE_FLYSKY_TUNING
					offset=(int16_t)FORCE_FLYSKY_TUNING;
				#endif
				break;
			case PROTO_HEIGHT:
				#ifdef FORCE_HEIGHT_TUNING
					offset=(int16_t)FORCE_HEIGHT_TUNING;
				#endif
				break;
			case PROTO_PELIKAN:
				#ifdef FORCE_PELIKAN_TUNING
					offset=(int16_t)FORCE_PELIKAN_TUNING;
				#endif
				break;
			case PROTO_KYOSHO:
				#ifdef FORCE_KYOSHO_TUNING
					offset=(int16_t)FORCE_KYOSHO_TUNING;
				#endif
				break;
			case PROTO_JOYSWAY:
				#ifdef FORCE_JOYSWAY_TUNING
					offset=(int16_t)FORCE_JOYSWAY_TUNING;
				#endif
				break;
			case PROTO_WFLY2:
				#ifdef FORCE_WFLY2_TUNING
					offset=(int16_t)FORCE_WFLY2_TUNING;
				#endif
				break;
			case PROTO_AFHDS2A:
			case PROTO_AFHDS2A_RX:
				#ifdef FORCE_AFHDS2A_TUNING
					offset=(int16_t)FORCE_AFHDS2A_TUNING;
				#endif
				break;
		}
	}
	if(offset==1024)	// Use channel 15 as an input
		offset=convert_channel_16b_nolimit(CH15,-300,300,false);

	if(old_offset==offset)	// offset is the same as before...
			return;
	old_offset=offset;

	// LO base frequency = 32e6*(bip+(bfp/(2^16)))
	uint8_t bip;	// LO base frequency integer part
	uint16_t bfp;	// LO base frequency fractional part
	offset++;		// as per datasheet, not sure why recommended, but that's a +1kHz drift only ...
	offset<<=1;
	if(offset < 0)
	{
		bip = 0x4a;	// 2368 MHz
		bfp = 0xffff + offset;
	}
	else
	{
		bip = 0x4b;	// 2400 MHz (default)
		bfp = offset;
	}
	A7105_WriteReg( A7105_11_PLL_III, bip);
	A7105_WriteReg( A7105_12_PLL_IV, (bfp >> 8) & 0xff);
	A7105_WriteReg( A7105_13_PLL_V, bfp & 0xff);
	//debugln("Channel: %d, offset: %d, bip: %2x, bfp: %4x", Channel_data[14], offset, bip, bfp);
}

static void __attribute__((unused)) A7105_SetVCOBand(uint8_t vb1, uint8_t vb2)
{	// Set calibration band value to best match
	uint8_t diff1, diff2;

	if (vb1 >= 4)
		diff1 = vb1 - 4;
	else
		diff1 = 4 - vb1;

	if (vb2 >= 4)
		diff2 = vb2 - 4;
	else
		diff2 = 4 - vb2;

	if (diff1 == diff2 || diff1 > diff2)
		A7105_WriteReg(A7105_25_VCO_SBCAL_I, vb1 | 0x08);
	else
		A7105_WriteReg(A7105_25_VCO_SBCAL_I, vb2 | 0x08);
}

#if defined(AFHDS2A_A7105_INO) || defined(AFHDS2A_RX_A7105_INO)
const uint8_t PROGMEM AFHDS2A_A7105_regs[] = {
	0xFF, 0x42 | (1<<5), 0x00, 0x25, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3c, 0x05, 0x00, 0x50,	// 00 - 0f
	0x9e, 0x4b, 0x00, 0x02, 0x16, 0x2b, 0x12, 0x4f, 0x62, 0x80, 0xFF, 0xFF, 0x2a, 0x32, 0xc3, 0x1f,				// 10 - 1f
	0x1e, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x3b, 0x00, 0x17, 0x47, 0x80, 0x03, 0x01, 0x45, 0x18, 0x00,				// 20 - 2f
	0x01, 0x0f // 30 - 31
};
#endif
#ifdef BUGS_A7105_INO
const uint8_t PROGMEM BUGS_A7105_regs[] = {
	0xFF, 0x42, 0x00, 0x15, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x05, 0x01, 0x50,	// 00 - 0f
	0x9e, 0x4b, 0x00, 0x02, 0x16, 0x2b, 0x12, 0x40, 0x62, 0x80, 0x80, 0x00, 0x0a, 0x32, 0xc3, 0x0f,	// 10 - 1f
	0x16, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x3b, 0x00, 0x0b, 0x47, 0x80, 0x03, 0x01, 0x45, 0x18, 0x00,	// 20 - 2f
	0x01, 0x0f // 30 - 31
};
#endif
#ifdef FLYSKY_A7105_INO
const uint8_t PROGMEM FLYSKY_A7105_regs[] = {
	0xff, 0x42, 0x00, 0x14, 0x00, 0xff, 0xff ,0x00, 0x00, 0x00, 0x00, 0x01, 0x21, 0x05, 0x00, 0x50,	// 00 - 0f
	0x9e, 0x4b, 0x00, 0x02, 0x16, 0x2b, 0x12, 0x00, 0x62, 0x80, 0x80, 0x00, 0x0a, 0x32, 0xc3, 0x0f,	// 10 - 1f
	0x13, 0xc3, 0x00, 0xff, 0x00, 0x00, 0x3b, 0x00, 0x17, 0x47, 0x80, 0x03, 0x01, 0x45, 0x18, 0x00,	// 20 - 2f
	0x01, 0x0f // 30 - 31
};
#endif
#ifdef HEIGHT_A7105_INO
const uint8_t PROGMEM HEIGHT_A7105_regs[] = {
	0xff, 0x42, 0x00, 0x07, 0x00, 0xff, 0xff ,0x00, 0x00, 0x00, 0x00, 0x01, 0x21, 0x05, 0x01, 0x50,	// 00 - 0f
	0x9e, 0x4b, 0x00, 0x02, 0x16, 0x2b, 0x12, 0x00, 0x62, 0x80, 0x80, 0x00, 0x0a, 0x32, 0xc3, 0x1f,	// 10 - 1f
	0x12, 0x00, 0x00, 0xff, 0x00, 0x00, 0x3a, 0x00, 0x3f, 0x47, 0x80, 0x03, 0x01, 0x45, 0x18, 0x00,	// 20 - 2f
	0x01, 0x0f // 30 - 31
};
#endif
#ifdef HUBSAN_A7105_INO
const uint8_t PROGMEM HUBSAN_A7105_regs[] = {
	0xFF, 0x63, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF ,0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x05, 0x04, 0xFF,	// 00 - 0f
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x2B, 0xFF, 0xFF, 0x62, 0x80, 0xFF, 0xFF, 0x0A, 0xFF, 0xFF, 0x07,	// 10 - 1f
	0x17, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x47, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,	// 20 - 2f
	0xFF, 0xFF // 30 - 31
};
#endif
#ifdef PELIKAN_A7105_INO
const uint8_t PROGMEM PELIKAN_A7105_regs[] = {
	0xff, 0x42, 0x00, 0x0F, 0x00, 0xff, 0xff ,0x00, 0x00, 0x00, 0x00, 0x01, 0x21, 0x05, 0x01, 0x50,	// 00 - 0f
	0x9e, 0x4b, 0x00, 0x02, 0x16, 0x2b, 0x12, 0x00, 0x62, 0x80, 0x80, 0x00, 0x0a, 0x32, 0xc3, 0x07,	// 10 - 1f
	0x16, 0x00, 0x00, 0xff, 0x00, 0x00, 0x3b, 0x00, 0x1f, 0x47, 0x80, 0x03, 0x01, 0x45, 0x18, 0x00,	// 20 - 2f
	0x01, 0x0f // 30 - 31
};
#endif
#ifdef KYOSHO_A7105_INO
const uint8_t PROGMEM KYOSHO_A7105_regs[] = {
	0xff, 0x42, 0xff, 0x25, 0x00, 0xff, 0xff ,0x00, 0x00, 0x00, 0x00, 0x01, 0x21, 0x05, 0x00, 0x50,	// 00 - 0f
	0x9e, 0x4b, 0x00, 0x02, 0x16, 0x2b, 0x12, 0x40, 0x62, 0x80, 0x80, 0x00, 0x0a, 0x32, 0x03, 0x1f,	// 10 - 1f
	0x1e, 0x00, 0x00, 0xff, 0x00, 0x00, 0x23, 0x70, 0x1F, 0x47, 0x80, 0x57, 0x01, 0x45, 0x19, 0x00,	// 20 - 2f
	0x01, 0x0f // 30 - 31
};
const uint8_t PROGMEM KYOSHO_HYPE_A7105_regs[] = {
	0xff, 0x42, 0x00, 0x10, 0xC0, 0xff, 0xff ,0x00, 0x00, 0x00, 0x00, 0x01, 0x09, 0x05, 0x01, 0x04,	// 00 - 0f
	0x9e, 0x4b, 0x00, 0x02, 0x16, 0x2b, 0x12, 0x00, 0x62, 0x80, 0x80, 0x00, 0x0a, 0x96, 0xc2, 0x1f,	// 10 - 1f
	0x12, 0x00, 0x00, 0xff, 0x00, 0x00, 0x3a, 0x00, 0x17, 0x47, 0x80, 0x03, 0x01, 0x45, 0x18, 0x00,	// 20 - 2f
	0x01, 0x0f // 30 - 31
};
#endif
#ifdef WFLY2_A7105_INO //A7106 values
const uint8_t PROGMEM WFLY2_A7105_regs[] = {
	0xff, 0x62, 0xff, 0x1F, 0x40, 0xff, 0xff ,0x00, 0x00, 0x00, 0x00, 0x33, 0x33, 0x05, 0x00, 0x64,	// 00 - 0f Changes: 0B:19->33, 0C:01,33
	0x9e, 0x4b, 0x00, 0x02, 0x16, 0x2b, 0x12, 0x40, 0x62, 0x80, 0x80, 0x00, 0x0a, 0x32, 0x03, 0x0f,	// 10 - 1f 1C:4A->0A
	0x12, 0x00, 0x00, 0xff, 0x00, 0x00, 0x23, 0x70, 0x15, 0x47, 0x80, 0x03, 0x01, 0x45, 0x18, 0x00,	// 20 - 2f 2B:77->03, 2E:19->18
	0x01, 0x0f // 30 - 31
};
#endif
#ifdef JOYSWAY_A7105_INO
const uint8_t PROGMEM JOYSWAY_A7105_regs[] = {
	0xff, 0x62, 0xff, 0x0F, 0x00, 0xff, 0xff ,0x00, 0x00, 0x05, 0x00, 0x01, 0x00, 0xF5, 0x00, 0x15,	// 00 - 0f
	0x9E, 0x4B, 0x00, 0x03, 0x56, 0x2B, 0x12, 0x4A, 0x02, 0x80, 0x80, 0x00, 0x0E, 0x91, 0x03, 0x0F,	// 10 - 1f
	0x16, 0x2A, 0x00, 0xff, 0xff, 0xff, 0x3A, 0x06, 0x1F, 0x47, 0x80, 0x01, 0x05, 0x45, 0x18, 0x00,	// 20 - 2f
	0x01, 0x0f // 30 - 31
};
#endif

#define ID_NORMAL 0x55201041
#define ID_PLUS   0xAA201041
void A7105_Init(void)
{
	uint8_t *A7105_Regs=0;
    uint8_t vco_calibration0, vco_calibration1;
	
	#ifdef JOYSWAY_A7105_INO
		if(protocol==PROTO_JOYSWAY)
		{
			A7105_Regs=(uint8_t*)JOYSWAY_A7105_regs;
		}
		else
	#endif
	#ifdef WFLY2_A7105_INO
		if(protocol==PROTO_WFLY2)
		{
			A7105_Regs=(uint8_t*)WFLY2_A7105_regs;
		}
		else
	#endif
	#ifdef HEIGHT_A7105_INO
		if(protocol==PROTO_HEIGHT)
		{
			A7105_Regs=(uint8_t*)HEIGHT_A7105_regs;
			A7105_WriteID(0x25A53C45);
		}
		else
	#endif
	#ifdef PELIKAN_A7105_INO
		if(protocol==PROTO_PELIKAN)
		{
			A7105_Regs=(uint8_t*)PELIKAN_A7105_regs;
			A7105_WriteID(0x06230623);
		}
		else
	#endif
	#ifdef BUGS_A7105_INO
		if(protocol==PROTO_BUGS)
			A7105_Regs=(uint8_t*)BUGS_A7105_regs;
		else
	#endif
	#ifdef HUBSAN_A7105_INO
		if(protocol==PROTO_HUBSAN)
		{
			A7105_WriteID(ID_NORMAL);
			A7105_Regs=(uint8_t*)HUBSAN_A7105_regs;
		}
		else
	#endif
		{
			A7105_WriteID(0x5475c52A);//0x2Ac57554
			#ifdef FLYSKY_A7105_INO
				if(protocol==PROTO_FLYSKY)
					A7105_Regs=(uint8_t*)FLYSKY_A7105_regs;
			#endif
			#if defined(AFHDS2A_A7105_INO) || defined(AFHDS2A_RX_A7105_INO)
				if(protocol==PROTO_AFHDS2A || protocol==PROTO_AFHDS2A_RX)
					A7105_Regs=(uint8_t*)AFHDS2A_A7105_regs;
			#endif
			#ifdef KYOSHO_A7105_INO
				if(protocol==PROTO_KYOSHO)
				{
					if(sub_protocol==KYOSHO_HYPE)
						A7105_Regs=(uint8_t*)KYOSHO_HYPE_A7105_regs;
					else //FHSS && SYNCRO
						A7105_Regs=(uint8_t*)KYOSHO_A7105_regs;
				}
			#endif
		}

	for (uint8_t i = 0; i < 0x32; i++)
	{
		uint8_t val=pgm_read_byte_near(&A7105_Regs[i]);
		#ifdef FLYSKY_A7105_INO
			if(protocol==PROTO_FLYSKY && sub_protocol==CX20)
			{
				if(i==0x0E) val=0x01;
				if(i==0x1F) val=0x1F;
				if(i==0x20) val=0x1E;
			}
		#endif
		#ifdef HEIGHT_A7105_INO
			if(protocol==PROTO_HEIGHT && sub_protocol==HEIGHT_8CH)
				if(i==0x03) val=0x0A;
		#endif
		if( val != 0xff)
			A7105_WriteReg(i, val);
	}
	A7105_Strobe(A7105_STANDBY);

	if(protocol==PROTO_KYOSHO && sub_protocol!=KYOSHO_HYPE)
	{//strange calibration...
		//IF Filter Bank Calibration
		A7105_WriteReg(A7105_02_CALC,0x0F);
		while(A7105_ReadReg(A7105_02_CALC));			// Wait for calibration to end
	//	A7105_ReadReg(A7105_22_IF_CALIB_I);
	//	A7105_ReadReg(A7105_24_VCO_CURCAL);
	//	A7105_ReadReg(25_VCO_SBCAL_I);
	//	A7105_ReadReg(1A_RX_GAIN_II);
	//	A7105_ReadReg(1B_RX_GAIN_III);
	}
	else
	{
		//IF Filter Bank Calibration
		A7105_WriteReg(A7105_02_CALC,1);
		while(A7105_ReadReg(A7105_02_CALC));			// Wait for calibration to end
	//	A7105_ReadReg(A7105_22_IF_CALIB_I);
	//	A7105_ReadReg(A7105_24_VCO_CURCAL);

		if(protocol!=PROTO_HUBSAN)
		{
			//VCO Current Calibration
			A7105_WriteReg(A7105_24_VCO_CURCAL,0x13);	//Recommended calibration from A7105 Datasheet
			//VCO Bank Calibration
			A7105_WriteReg(A7105_26_VCO_SBCAL_II,0x3b);	//Recommended calibration from A7105 Datasheet
		}

		//VCO Bank Calibrate channel 0
		A7105_WriteReg(A7105_0F_CHANNEL, 0);
		A7105_WriteReg(A7105_02_CALC,2);
		while(A7105_ReadReg(A7105_02_CALC));			// Wait for calibration to end
		vco_calibration0 = A7105_ReadReg(A7105_25_VCO_SBCAL_I);
		
		//VCO Bank Calibrate channel A0
		A7105_WriteReg(A7105_0F_CHANNEL, 0xa0);
		A7105_WriteReg(A7105_02_CALC, 2);
		while(A7105_ReadReg(A7105_02_CALC));			// Wait for calibration to end
		vco_calibration1 = A7105_ReadReg(A7105_25_VCO_SBCAL_I);

		if(protocol==PROTO_BUGS || protocol==PROTO_WFLY2)
			A7105_SetVCOBand(vco_calibration0 & 0x07, vco_calibration1 & 0x07);	// Set calibration band value to best match
		else
			if(protocol!=PROTO_HUBSAN)
			{
				switch(protocol)
				{
					case PROTO_FLYSKY:
						vco_calibration1=0x08;
						break;
					case PROTO_HEIGHT:
						vco_calibration1=0x02;
						break;
					case PROTO_PELIKAN:
						if(sub_protocol == PELIKAN_SCX24)
						{
							vco_calibration1=0x0A;
							break;
						}
					case PROTO_KYOSHO: //sub_protocol Hype
						vco_calibration1=0x0C;
						break;
					case PROTO_JOYSWAY:
						vco_calibration1=0x09;
						break;
					default:
						vco_calibration1=0x0A;
						break;
				}
				A7105_WriteReg(A7105_25_VCO_SBCAL_I,vco_calibration1);	//Reset VCO Band calibration
			}
	}
	A7105_SetTxRxMode(TX_EN);
	A7105_SetPower();

	#ifdef USE_A7105_CH15_TUNING
		A7105_AdjustLOBaseFreq(0);
	#endif
	
	A7105_Strobe(A7105_STANDBY);
}
