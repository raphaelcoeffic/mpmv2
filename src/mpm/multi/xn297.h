#pragma once

// The MULTI-Module's XN297 emulation (XN297_EMU.ino, iface_xn297.h) on the
// CC2652 radio: same functions, frame.c does the XN297 framing (address,
// scrambling, bit order, CRC with xorout, enhanced mode)

#include "mpm_compat.h"

#define XN297_UNSCRAMBLED false
#define XN297_SCRAMBLED true
#define XN297_CRCDIS false
#define XN297_CRCEN true
#define XN297_1M false
#define XN297_250K true
#define XN297_NRF false
#define XN297_CC2500 true

// force_nrf is optional, as in C++
#define XN297_Configure(...) _XN297_CONFIGURE(__VA_ARGS__, false, 0)
#define _XN297_CONFIGURE(crc_en, scramble_en, bitrate, force_nrf, ...) \
  xn297_configure(crc_en, scramble_en, bitrate, force_nrf)
bool xn297_configure(bool crc_en, bool scramble_en, bool bitrate, bool force_nrf);

void XN297_SetTXAddr(const uint8_t* addr, uint8_t len);
void XN297_SetRXAddr(const uint8_t* addr, uint8_t rx_packet_len);
void XN297_SetTxRxMode(enum TXRX_State mode);
void XN297_WritePayload(uint8_t* msg, uint8_t len);
void XN297_WriteEnhancedPayload(uint8_t* msg, uint8_t len, uint8_t noack);
void XN297_ReSendPayload();
bool XN297_IsRX();
bool XN297_ReadPayload(uint8_t* msg, uint8_t len);
uint8_t XN297_ReadEnhancedPayload(uint8_t* msg, uint8_t len);
bool XN297_IsPacketSent();
void XN297_HoppingCalib(uint8_t num_freq);
void XN297_Hopping(uint8_t index);
void XN297_RFChannel(uint8_t number);
void XN297_SetPower();
void XN297_SetFreqOffset();
