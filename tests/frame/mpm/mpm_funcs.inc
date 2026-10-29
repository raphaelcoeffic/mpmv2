// From the DIY-Multiprotocol-TX-Module XN297_EMU.ino (GPL v3 or later, master 91199e1),
// kept as close to the original as possible to test frame.c against it

static void __attribute__((unused)) XN297_WritePayload(uint8_t* msg, uint8_t len)
{
	uint8_t buf[32];
	uint8_t last = 0;

	if (xn297_rf == XN297_NRF && xn297_addr_len < 4 && xn297_rf == XN297_NRF)
	{ // If address length (which is defined by receiver address length) is less than 4 the TX address can't fit the preamble, so the last byte goes here
		buf[last++] = 0x55;
	}

	// address
	crc = 0xFFFF;
	XN297_scramble_init();
	for (uint8_t i = 0; i < xn297_addr_len; ++i)
		buf[last++] = XN297_crc_scramble(xn297_tx_addr[xn297_addr_len-i-1]);

	// payload
	for (uint8_t i = 0; i < len; ++i)
		buf[last++] = XN297_crc_scramble(bit_reverse(msg[i]));	// bit-reverse bytes in packet

	// crc
	if (xn297_crc)
	{
		crc = ~crc ^ (xn297_lfsr >> 16);
		buf[last++] = crc >> 8;
		buf[last++] = crc & 0xff;
	}

	// send packet
	XN297_SendPayload(buf, last);
}

static void __attribute__((unused)) XN297_WriteEnhancedPayload(uint8_t* msg, uint8_t len, uint8_t noack)
{
	uint8_t buf[32];
	uint8_t last = 0;
	static uint8_t pid=0;

	if (xn297_rf == XN297_NRF && xn297_addr_len < 4)
	{ // If address length (which is defined by receiver address length) is less than 4 the TX address can't fit the preamble, so the last byte goes here
		buf[last++] = 0x55;
	}

	// address
	crc = xn297_scramble_enabled ? 0xFFFF : 0xB5D2;	// unscrambled: no xorout
	XN297_scramble_init();
	for (uint8_t i = 0; i < xn297_addr_len; ++i)
		buf[last++] = XN297_crc_scramble(xn297_tx_addr[xn297_addr_len-i-1]);

	// pcf
	buf[last++] = XN297_crc_scramble((len << 1) | (pid>>1));
	buf[last] = (pid << 7) | (noack << 6);

	// payload
	if(len)
	{
		buf[last] = XN297_crc_scramble(buf[last] | (bit_reverse(msg[0]) >> 2)); // first 6 bit of payload

		for (uint8_t i = 0; i < len-1; ++i)
		{
			last++;
			buf[last] = XN297_crc_scramble((bit_reverse(msg[i]) << 6) | (bit_reverse(msg[i+1]) >> 2));
		}

		last++;
		buf[last] = bit_reverse(msg[len-1]) << 6; // last 2 bit of payload
	}
	crc16_update( buf[last], 2);
	buf[last] ^= (xn297_lfsr >> 24) & 0xc0;

	// crc
	if (xn297_crc)
	{
		if (xn297_scramble_enabled)
			crc = ~crc ^ (xn297_lfsr >> 14);

		buf[last++] |= (crc >> 8) >> 2;
		buf[last++] = ((crc >> 8) << 6) | ((crc & 0xff) >> 2);
		buf[last++] = (crc & 0xff) << 6;
	}
	pid++;
	pid &= 0x03;

	// send packet
	XN297_SendPayload(buf, last);
}

static bool __attribute__((unused)) XN297_ReadPayload(uint8_t* msg, uint8_t len)
{ //!!! Don't forget if using CRC to do a +2 on the received packet length (NRF24L01_11_RX_PW_Px !!! or CC2500_06_PKTLEN)
	uint8_t buf[32];

	// Read payload
	XN297_ReceivePayload(buf, len);

	// Decode payload, calculate CRC
	crc = 0xFFFF;
	XN297_scramble_init();
	for (uint8_t i = 0; i < xn297_addr_len; ++i)
		XN297_unscramble_crc(xn297_rx_addr[xn297_addr_len-i-1]);
	for(uint8_t i=0; i<len; i++)
		msg[i] = bit_reverse(XN297_unscramble_crc(buf[i]));

	if (!xn297_crc)
		return true;	// No CRC so OK by default...

	crc = ~crc ^ (xn297_lfsr >> 16);
	//test
	if( (crc >> 8) == buf[len] && (crc & 0xff) == buf[len+1])
		return true;	// CRC  OK
	return false;		// CRC NOK
}

static uint8_t __attribute__((unused)) XN297_ReadEnhancedPayload(uint8_t* msg, uint8_t len)
{ //!!! Don't forget do a +2 and if using CRC add +4 on any of the used NRF24L01_11_RX_PW_Px !!!
	uint8_t buffer[32];
	uint8_t pcf_size;							// pcf payload size

	// Read payload
	XN297_ReceivePayload(buffer, len+2);		// Read pcf + payload + CRC
	// Decode payload, calculate CRC
#ifdef XN297DUMP_NRF24L01_INO
	bool xorout = true;
#else
	bool xorout = xn297_scramble_enabled;		// unscrambled: no xorout
#endif
	crc = xorout ? 0xFFFF : 0xB5D2;
	XN297_scramble_init();
	for (uint8_t i = 0; i < xn297_addr_len; ++i)
		XN297_unscramble_crc(xn297_rx_addr[xn297_addr_len-i-1]);
	pcf_size = XN297_unscramble_crc(buffer[0]) >> 1;
	if(pcf_size>32)
		return 255;								// Error
	for(uint8_t i=0; i< pcf_size; i++)
	{	// payload is shifted by 2 bits: unscramble with bits 29..22 of the next scramble bits
		msg[i] = bit_reverse(((buffer[i+1] << 2) | (buffer[i+2] >> 6)) ^ (xn297_lfsr >> 22));
		XN297_unscramble_crc(buffer[i+1]);
	}
	crc16_update( (buffer[pcf_size+1] ^ (xn297_lfsr >> 24)) & 0xc0, 2);

	if (!xn297_crc)
		return pcf_size;						// No CRC so OK by default...

	if (xorout)
		crc = ~crc ^ (xn297_lfsr >> 14);
	uint16_t crcxored=(buffer[pcf_size+1]<<10)|(buffer[pcf_size+2]<<2)|(buffer[pcf_size+3]>>6) ;
	if( crc == crcxored)
		return pcf_size;					// CRC  OK
	return 255;								// CRC NOK
}
