#pragma once

// Futaba T-FHSS (Surface) packets and hops, from the R334SBS receiver's
// firmware (the tracing notes in doc/futaba*, not in the repository).
// CC2500 384 kbps MSK, sync D391, 14 bytes: 12 bytes XOR a key, then the
// CRC-16/MCRF4XX of the 12 plain bytes, low byte first. 31 channels
// n = 1..31 at 2407.5 + 2 (n - 1) MHz (CC2500 CHANNR 5 (n - 1)).
//
// The key: 00 00 ID0..3 x n b5..b8, from the bind packet but n, the
// channel. Bytes 0 and 1 are plain: byte 0 bits 0..4 = the type, byte 0
// bits 5..7 and byte 1 bits 0..3 = the hop row (the receiver takes it
// from the first packet it hears). The next channel is the one after the
// current in the row (tfhss_next()).

#include <stdint.h>

#define TFHSS_CHANNELS 31
#define TFHSS_PACKET_LEN 14
#define TFHSS_DATA_LEN 12
// Bind: plain packets on channel 1
#define TFHSS_BIND_CHANNEL 1
// Types: 0 / 1 = servo data (the receiver takes one or the other, set at
// bind: T-FHSS / SR?), 8 / 9 = bind, 11 = settings; 16 and up = receiver
// replies (16 sensors, 24 / 25 bind, 27..29 settings)
#define TFHSS_TYPE_REPLY 16
// Rows 0..30; TFHSS_ROW_LIST: the 16-channel list of the receiver's other
// mode (set by a type 8 / 9 packet on the link, purpose unknown)
#define TFHSS_ROWS 31
#define TFHSS_ROW_LIST 0xFF

// Channel n (1..31) at position pos (0..30) of a row
uint8_t tfhss_channel(uint8_t row, uint8_t pos);
// The channel after n, as the receiver hops: the row's first if n is not
// in it
uint8_t tfhss_next(uint8_t row, uint8_t n);

static inline uint8_t tfhss_type(const uint8_t* p) { return p[0] & 0x1F; }
static inline uint8_t tfhss_row(const uint8_t* p) { return p[0] >> 5 | (p[1] & 0x0F) << 3; }

// CRC-16/MCRF4XX: tfhss_crc(p, n, 0xFFFF)
uint16_t tfhss_crc(const uint8_t* p, uint8_t n, uint16_t crc);

// Packet from the 12 plain bytes and the key (key[7] = the channel)
void tfhss_packet(uint8_t* out, const uint8_t* plain, const uint8_t* key);

// A packet's CRC syndrome: CRC of the 12 sent bytes XOR its CRC field, the
// key's CRC from 0 (0 for a plain packet). Without the channel's part
// (key[7] = n): the same for every packet of one key, whatever the channel
uint16_t tfhss_syndrome(const uint8_t* p);
uint16_t tfhss_key_syndrome(const uint8_t* p, uint8_t n);

// T-FHSS Air (T18SZ transmitter firmware, doc/futaba*): CC2500 128 kbps
// 2-FSK, 38 kHz deviation, sync D391, the same channels, rows and list.
// TX: 17 bytes, a frame every 15.04 ms: byte 0 plain (row << 3 | flags;
// row 31 = a link frame, sent plain on the list), bytes 1..14 XOR the key
// 00 I3 I2 I1 I0 x n I3 I2 I1 I0 x n I3 I2 (I = TX ID, n = channel), then
// the CRC-16/MCRF4XX of the 15 plain bytes. The receiver's reply: 20 bytes
// on the next channel 5.2..9.2 ms after the TX packet, 18 XOR a key from
// the receiver's ID (n at 6 and 12 as well), CRC of the 18 plain bytes
#define TFHSS_AIR_LEN 17
#define TFHSS_AIR_REPLY_LEN 20
#define TFHSS_AIR_DATA_LEN 15
#define TFHSS_AIR_ROW_LINK 31
#define TFHSS_AIR_PERIOD_US 15040

static inline uint8_t tfhss_air_row(const uint8_t* p) { return p[0] >> 3; }

// Packet from the 15 plain bytes and the key (key[6] = key[12] = the channel)
void tfhss_air_packet(uint8_t* out, const uint8_t* plain, const uint8_t* key);
// As tfhss_key_syndrome(), over a len byte Air packet (CRC in the last two)
uint16_t tfhss_air_key_syndrome(const uint8_t* p, uint8_t len, uint8_t n);
