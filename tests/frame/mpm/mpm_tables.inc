// From the DIY-Multiprotocol-TX-Module XN297_EMU.ino (GPL v3 or later, master d3293af),
// kept as close to the original as possible to test frame.c against it

// xn297 address / pcf / payload scramble table
const uint8_t xn297_scramble[] = {
    0xE3, 0xB1, 0x4B, 0xEA, 0x85, 0xBC, 0xE5, 0x66,
    0x0D, 0xAE, 0x8C, 0x88, 0x12, 0x69, 0xEE, 0x1F,
    0xC7, 0x62, 0x97, 0xD5, 0x0B, 0x79, 0xCA, 0xCC,
    0x1B, 0x5D, 0x19, 0x10, 0x24, 0xD3, 0xDC, 0x3F,
    0x8E, 0xC5, 0x2F, 0xAA, 0x16, 0xF3, 0x95 };

// XN297 CRC: CRC-16/CCITT (init 0xFFFF, xorout 0xFFFF) over the unscrambled
// address, pcf and payload, sent scrambled. The emulation computes it from 0xB5D2
// over the bits as sent: the xorout is the difference for a CRC over 'bits' bits,
// the CRC of the scramble bits plus the scramble of the CRC bits.
static uint16_t __attribute__((unused)) XN297_crc_xorout(bool scramble, uint16_t bits)
{
	uint16_t xorout = 0xFFFF ^ 0xB5D2;
	uint8_t lfsr = 0x71, s;					// scramble bits (xn297_scramble[]): 1110001, then s[i] = s[i-7] ^ s[i-3]
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
