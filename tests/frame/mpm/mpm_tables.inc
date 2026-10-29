// From the DIY-Multiprotocol-TX-Module XN297_EMU.ino (GPL v3 or later, master 91199e1),
// kept as close to the original as possible to test frame.c against it

// iface_xn297.h
#define	XN297_SCRAMBLE_SEED 0xE3B14BE0	// whitening x^7 + x^3 + 1, seed 0x71: first 28 bits

// XN297 whitening: x^7 + x^3 + 1, seed 0x71, MSB first (E3 B1 4B EA 85 ...).
// Generated a byte at a time with (x^7 + x^3 + 1)^4 = x^28 + x^12 + 1: s[i] = s[i-28] ^ s[i-12].
// xn297_lfsr holds the next 28 bits in bits 31..4, 0 when scrambling is off.
uint32_t xn297_lfsr;

static uint8_t __attribute__((unused)) XN297_scramble_byte()
{
	uint8_t s = xn297_lfsr >> 24;
	xn297_lfsr = (xn297_lfsr << 8) | (uint8_t)(s ^ (xn297_lfsr >> 8)) << 4;
	return s;
}

static void __attribute__((unused)) XN297_scramble_init()
{
	xn297_lfsr = xn297_scramble_enabled ? XN297_SCRAMBLE_SEED : 0;
}

// XN297 CRC: CRC-16/CCITT (init 0xFFFF, xorout 0xFFFF) over the unscrambled
// address, pcf and payload, sent scrambled: XORed with the scramble bits that follow,
// (xn297_lfsr >> 16) once the last byte has been scrambled.
// Returns the scrambled byte, crc updated with the unscrambled one.
static uint8_t __attribute__((unused)) XN297_crc_scramble(uint8_t b)
{
	crc16_update(b, 8);
	return b ^ XN297_scramble_byte();
}

// Returns the unscrambled byte, crc updated with it.
static uint8_t __attribute__((unused)) XN297_unscramble_crc(uint8_t b)
{
	b ^= XN297_scramble_byte();
	crc16_update(b, 8);
	return b;
}

#ifdef XN297DUMP_NRF24L01_INO
// XN297Dump computes the CRC from 0xB5D2 over the bits as sent: the xorout is the
// difference for a CRC over 'bits' bits, the CRC of the scramble bits plus the
// scramble of the CRC bits.
static uint16_t __attribute__((unused)) XN297_crc_xorout(bool scramble, uint16_t bits)
{
	uint16_t xorout = 0xFFFF ^ 0xB5D2;
	uint8_t lfsr = 0x71, s;					// scramble bits: 1110001, then s[i] = s[i-7] ^ s[i-3]
	for(uint16_t i = 0; i < bits + 16; i++)
	{
		s = scramble ? lfsr >> 6 : 0;
		lfsr = ((lfsr << 1) | (((lfsr >> 6) ^ (lfsr >> 2)) & 1)) & 0x7F;
		if(i < bits)
		{
			if((xorout >> 15) ^ s)
				xorout = (xorout << 1) ^ 0x1021;
			else
				xorout <<= 1;
		}
		else
			xorout ^= (uint16_t)s << (bits + 15 - i);
	}
	return ~xorout;
}
#endif
