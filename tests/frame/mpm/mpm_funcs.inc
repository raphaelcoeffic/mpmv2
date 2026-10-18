// From the DIY-Multiprotocol-TX-Module XN297_EMU.ino (GPL v3 or later), kept
// as close to the original as possible to test frame.c against it

static void __attribute__((unused)) XN297_WritePayload(uint8_t* msg, uint8_t len)
{
	uint8_t buf[32];
	uint8_t last = 0;

	if (xn297_rf == XN297_NRF && xn297_addr_len < 4 && xn297_rf == XN297_NRF)
	{ // If address length (which is defined by receiver address length) is less than 4 the TX address can't fit the preamble, so the last byte goes here
		buf[last++] = 0x55;
	}

	// address
	for (uint8_t i = 0; i < xn297_addr_len; ++i)
	{
		buf[last] = xn297_tx_addr[xn297_addr_len-i-1];
		if(xn297_scramble_enabled)
			buf[last] ^=  xn297_scramble[i];
		last++;
	}

	// payload
	for (uint8_t i = 0; i < len; ++i)
	{
		// bit-reverse bytes in packet
		buf[last] = bit_reverse(msg[i]);
		if(xn297_scramble_enabled)
			buf[last] ^= xn297_scramble[xn297_addr_len+i];
		last++;
	}

	// crc
	if (xn297_crc)
	{
		uint8_t offset = (xn297_addr_len < 4  && xn297_rf == XN297_NRF) ? 1 : 0;
		crc = 0xb5d2;
		for (uint8_t i = offset; i < last; ++i)
			crc16_update( buf[i], 8);
		if(xn297_scramble_enabled)
			crc ^= pgm_read_word(&xn297_crc_xorout_scrambled[xn297_addr_len - 3 + len]);
		else
			crc ^= pgm_read_word(&xn297_crc_xorout[xn297_addr_len - 3 + len]);
		buf[last++] = crc >> 8;
		buf[last++] = crc & 0xff;
	}

	// send packet
	XN297_SendPayload(buf, last);
}
static void __attribute__((unused)) XN297_WriteEnhancedPayload(uint8_t* msg, uint8_t len, uint8_t noack)
{
	uint8_t buf[32];
	uint8_t scramble_index=0;
	uint8_t last = 0;
	static uint8_t pid=0;

	if (xn297_rf == XN297_NRF && xn297_addr_len < 4)
	{ // If address length (which is defined by receiver address length) is less than 4 the TX address can't fit the preamble, so the last byte goes here
		buf[last++] = 0x55;
	}

	// address
	for (uint8_t i = 0; i < xn297_addr_len; ++i)
	{
		buf[last] = xn297_tx_addr[xn297_addr_len-i-1];
		if(xn297_scramble_enabled)
			buf[last] ^= xn297_scramble[scramble_index++];
		last++;
	}

	// pcf
	buf[last] = (len << 1) | (pid>>1);
	if(xn297_scramble_enabled)
		buf[last] ^= xn297_scramble[scramble_index++];
	last++;
	buf[last] = (pid << 7) | (noack << 6);

	// payload
	if(len)
	{
		buf[last]|= bit_reverse(msg[0]) >> 2; // first 6 bit of payload
		if(xn297_scramble_enabled)
			buf[last] ^= xn297_scramble[scramble_index++];

		for (uint8_t i = 0; i < len-1; ++i)
		{
			last++;
			buf[last] = (bit_reverse(msg[i]) << 6) | (bit_reverse(msg[i+1]) >> 2);
			if(xn297_scramble_enabled)
				buf[last] ^= xn297_scramble[scramble_index++];
		}

		last++;
		buf[last] = bit_reverse(msg[len-1]) << 6; // last 2 bit of payload
	}
	if(xn297_scramble_enabled)
		buf[last] ^= xn297_scramble[scramble_index++] & 0xc0;

	// crc
	if (xn297_crc)
	{
		uint8_t offset = (xn297_addr_len < 4 && xn297_rf == XN297_NRF) ? 1 : 0;
		crc = 0xb5d2;
		for (uint8_t i = offset; i < last; ++i)
			crc16_update( buf[i], 8);
		crc16_update( buf[last] & 0xc0, 2);
		if (xn297_scramble_enabled)
			crc ^= pgm_read_word(&xn297_crc_xorout_scrambled_enhanced[xn297_addr_len-3+len]);
		//else
		//	crc ^= pgm_read_word(&xn297_crc_xorout_enhanced[xn297_addr_len - 3 + len]);

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

	// Decode payload
	for(uint8_t i=0; i<len; i++)
	{
		uint8_t b_in=buf[i];
		if(xn297_scramble_enabled)
			b_in ^= xn297_scramble[i+xn297_addr_len];
		msg[i] = bit_reverse(b_in);
	}

	if (!xn297_crc)
		return true;	// No CRC so OK by default...

	// Calculate CRC
	crc = 0xb5d2;
	//process address
	for (uint8_t i = 0; i < xn297_addr_len; ++i)
		crc16_update( xn297_rx_addr[xn297_addr_len-i-1], 8);
	//process payload
	for (uint8_t i = 0; i < len; ++i)
		crc16_update( buf[i], 8);
	//xorout
	if(xn297_scramble_enabled)
		crc ^= pgm_read_word(&xn297_crc_xorout_scrambled[xn297_addr_len - 3 + len]);
	else
		crc ^= pgm_read_word(&xn297_crc_xorout[xn297_addr_len - 3 + len]);
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
	// Decode payload
	pcf_size = buffer[0];
	if(xn297_scramble_enabled)
		pcf_size ^= xn297_scramble[xn297_addr_len];
	pcf_size = pcf_size >> 1;
	if(pcf_size>32)
		return 255;								// Error
	for(uint8_t i=0; i< pcf_size; i++)
	{
		msg[i] = bit_reverse((buffer[i+1] << 2) | (buffer[i+2] >> 6));
		if(xn297_scramble_enabled)
			msg[i] ^= bit_reverse((xn297_scramble[xn297_addr_len+i+1] << 2) | 
									(xn297_scramble[xn297_addr_len+i+2] >> 6));
	}

	if (!xn297_crc)
		return pcf_size;						// No CRC so OK by default...

	// Calculate CRC
	crc = 0xb5d2;
	//process address
	for (uint8_t i = 0; i < xn297_addr_len; ++i)
		crc16_update( xn297_rx_addr[xn297_addr_len-i-1], 8);
	//process payload
	for (uint8_t i = 0; i < pcf_size+1; ++i)
		crc16_update( buffer[i], 8);
	crc16_update( buffer[pcf_size+1] & 0xc0, 2);
	//xorout
	if (xn297_scramble_enabled)
		crc ^= pgm_read_word(&xn297_crc_xorout_scrambled_enhanced[xn297_addr_len-3+pcf_size]);
#ifdef XN297DUMP_NRF24L01_INO
	else
		crc ^= pgm_read_word(&xn297_crc_xorout_enhanced[xn297_addr_len - 3 + pcf_size]);
#endif
	uint16_t crcxored=(buffer[pcf_size+1]<<10)|(buffer[pcf_size+2]<<2)|(buffer[pcf_size+3]>>6) ;
	if( crc == crcxored)
		return pcf_size;					// CRC  OK
	return 255;								// CRC NOK
}
