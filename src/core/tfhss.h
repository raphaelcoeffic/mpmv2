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
