/*
 This project is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Multiprotocol is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Multiprotocol.  If not, see <http://www.gnu.org/licenses/>.
 */
//**************************
// Telemetry serial code   *
//**************************
// mpm-v2: Telemetry.ino, MULTI_TELEMETRY only: the protocols' telemetry to
// the radio. Changes: Serial_write() and multi_send_header() on the core's
// telemetry output, the status (multi_send_status()) and MULTI_SYNC left to
// the core, TelemetryUpdate() without the status, frsky_*_user_frame() not
// static (other files use them)
#include "mpm_compat.h"
#include "../mpm_frame.h"
#include "../mpm_port.h"

static void Serial_write(uint8_t byte) { mpm_port_write(&byte, 1); }

#if defined TELEMETRY

uint8_t RetrySequence ;

#ifdef MULTI_TELEMETRY
	uint32_t lastMulti = 0;
	#define MULTI_TIME				500	//in ms
	#ifdef MULTI_SYNC
		#define INPUT_SYNC_TIME			100	//in ms
		#define INPUT_ADDITIONAL_DELAY	100	// in 10µs, 100 => 1000 µs
		uint32_t lastInputSync = 0;
		uint16_t inputDelay = 0;
	#endif // MULTI_SYNC
#endif // MULTI_TELEMETRY

#if defined SPORT_TELEMETRY	
	#define FRSKY_SPORT_PACKET_SIZE   8
	#define FX_BUFFERS	4
	uint8_t Sport_Data = 0;
	uint8_t pktx1[FRSKY_SPORT_PACKET_SIZE*FX_BUFFERS];

	// Store for out of sequence packet
	uint8_t FrSkyX_RX_ValidSeq ;
	struct t_FrSkyX_RX_Frame
	{
		boolean valid;
		uint8_t count;
		uint8_t payload[6];
	} ;

	// Store for FrskyX telemetry
	struct t_FrSkyX_RX_Frame FrSkyX_RX_Frames[4] ;
	uint8_t FrSkyX_RX_NextFrame=0;
#endif // SPORT_TELEMETRY

#if defined HUB_TELEMETRY
	#define USER_MAX_BYTES 6
	uint8_t prev_index;

	struct t_FrSkyD_User_Frame
	{
		uint8_t ID;
		uint8_t low;
		uint8_t high;
	} FrSkyD_User_Frame[8];
	uint8_t FrSkyD_User_Frame_Start=0, FrSkyD_User_Frame_End=0;
#endif // HUB_TELEMETRY

#define START_STOP	0x7e
#define BYTESTUFF	0x7d
#define STUFF_MASK	0x20
#define MAX_PKTX	10
uint8_t pktx[MAX_PKTX];
uint8_t frame[18];

#ifdef MULTI_TELEMETRY
static void multi_send_header(uint8_t type, uint8_t len)
{
	uint8_t buf[MPM_TELEM_HDR_LEN];
	mpm_port_write(buf, mpm_telem_header(buf, type, len));
}

#ifdef DSM_TELEMETRY
	void DSM_frame()
	{
		if (packet_in[0] == 0x80)
		{
			multi_send_header(MULTI_TELEMETRY_DSMBIND, 10);
			for (uint8_t i = 1; i < 11; i++) 	// 10 bytes of DSM bind response
				Serial_write(packet_in[i]);

		}
		else
		{
			multi_send_header(MULTI_TELEMETRY_DSM, 17);
			for (uint8_t i = 0; i < 17; i++)	// RSSI value followed by 16 bytes of telemetry data
				Serial_write(packet_in[i]);
		}
	}
#endif

#ifdef SCANNER_TELEMETRY
	void spectrum_scanner_frame()
	{
		multi_send_header(MULTI_TELEMETRY_SCANNER, SCAN_CHANS_PER_PACKET + 1);
		Serial_write(packet_in[0]);					// start channel
		for(uint8_t ch = 0; ch < SCAN_CHANS_PER_PACKET; ch++)
			Serial_write(packet_in[ch+1]);			// RSSI power levels
	}
#endif

#if defined (FRSKY_RX_TELEMETRY) || defined (AFHDS2A_RX_TELEMETRY) || defined (BAYANG_RX_TELEMETRY) || defined (DSM_RX_CYRF6936_INO)
	void receiver_channels_frame()
	{
		uint16_t len = packet_in[3] * 11;			// 11 bit per channel
		if (len % 8 == 0)
			len = 4 + (len / 8);
		else
			len = 5 + (len / 8);
		multi_send_header(MULTI_TELEMETRY_RX_CHANNELS, len);
		for (uint8_t i = 0; i < len; i++)
			Serial_write(packet_in[i]);				// pps, rssi, ch start, ch count, 16x ch data
	}
#endif

#ifdef AFHDS2A_FW_TELEMETRY
	void AFHDSA_short_frame()
	{
		multi_send_header(packet_in[29]==0xAA?MULTI_TELEMETRY_AFHDS2A:MULTI_TELEMETRY_AFHDS2A_AC, 29);
		for (uint8_t i = 0; i < 29; i++)			// RSSI value followed by 4*7 bytes of telemetry data
			Serial_write(packet_in[i]);
	}
#endif

#ifdef HITEC_FW_TELEMETRY
	void HITEC_short_frame()
	{
		multi_send_header(MULTI_TELEMETRY_HITEC, 8);
		for (uint8_t i = 0; i < 8; i++)				// TX RSSI and TX LQI values followed by frame number and 5 bytes of telemetry data
			Serial_write(packet_in[i]);
	}
#endif

#ifdef HOTT_FW_TELEMETRY
	void HOTT_short_frame()
	{
		multi_send_header(MULTI_TELEMETRY_HOTT, 15);
		for (uint8_t i = 0; i < 15; i++)			// TX RSSI and TX LQI values followed by frame number and telemetry data
			Serial_write(packet_in[i]);
	}
#endif

#ifdef RLINK_HUB_TELEMETRY
	void RLINK_raw_frame()
	{
		multi_send_header(MULTI_TELEMETRY_RLINK, packet_in[0]);
		for (uint8_t i = 1; i <= packet_in[0]; i++)	// raw DumboRC reply payload
			Serial_write(packet_in[i]);
	}
#endif

static void multi_send_frskyhub()
{
	multi_send_header(MULTI_TELEMETRY_HUB, 9);
	for (uint8_t i = 0; i < 9; i++)
		Serial_write(frame[i]);
}

#endif //MULTI_TELEMETRY

void frskySendStuffed()
{
	Serial_write(START_STOP);
	for (uint8_t i = 0; i < 9; i++)
	{
		if ((frame[i] == START_STOP) || (frame[i] == BYTESTUFF))
		{
			Serial_write(BYTESTUFF);
			frame[i] ^= STUFF_MASK;
		}
		Serial_write(frame[i]);
	}
	Serial_write(START_STOP);
}

bool frsky_process_telemetry(uint8_t *buffer,uint8_t len)
{
	if(protocol!=PROTO_FRSKY_R9)
	{
		if(buffer[1] != rx_tx_addr[3] || buffer[2] != rx_tx_addr[2] || len != buffer[0] + 3 )
			return false;										// Bad address or length...
		// RSSI and LQI are the 2 last bytes
		TX_RSSI = buffer[len-2];
		if(TX_RSSI >=128)
			TX_RSSI -= 128;
		else
			TX_RSSI += 128;
	}
	telemetry_link|=1;								// Telemetry data is available

#if defined FRSKYD_CC2500_INO
	if (protocol==PROTO_FRSKYD)
	{
		TX_LQI = buffer[len-1]&0x7F;
		//Save current buffer
		for (uint8_t i=3;i<len-2;i++)
			telemetry_in_buffer[i]=buffer[i];	// Buffer telemetry values to be sent
	
		//Check incoming telemetry sequence
		if(telemetry_in_buffer[6]>0 && telemetry_in_buffer[6]<=10)
		{ //Telemetry length ok
			if ( ( telemetry_in_buffer[7] & 0x1F ) == (telemetry_counter & 0x1F) )
			{//Sequence is ok
				uint8_t topBit = 0 ;
				if ( telemetry_counter & 0x80 )
					if ( ( telemetry_counter & 0x1F ) != RetrySequence )
						topBit = 0x80 ;
				telemetry_counter = ( (telemetry_counter+1)%32 ) | topBit ;	// Request next telemetry frame
			}
			else
			{//Incorrect sequence
				RetrySequence = telemetry_in_buffer[7] & 0x1F ;
				telemetry_counter |= 0x80 ;
				telemetry_in_buffer[6]=0 ;			// Discard current packet and wait for retransmit
			}
		}
		else
			telemetry_in_buffer[6]=0; 				// Discard packet
	}
#endif

#if defined SPORT_TELEMETRY && (defined FRSKYX_CC2500_INO || defined FRSKYR9_SX1276_INO)
	if (protocol==PROTO_FRSKYX||protocol==PROTO_FRSKYX2)
	{
		/*Telemetry frames(RF) SPORT info 
		15 bytes payload
		SPORT frame valid 6+3 bytes
		[00] PKLEN  0E 0E 0E 0E 
		[01] TXID1  DD DD DD DD 
		[02] TXID2  6D 6D 6D 6D 
		[03] CONST  02 02 02 02 
		[04] RS/RB  2C D0 2C CE	//D0;CE=2*RSSI;....2C = RX battery voltage(5V from Bec)
		[05] HD-SK  03 10 21 32	//TX/RX telemetry hand-shake bytes
		[06] NO.BT  00 00 06 03	//No.of valid SPORT frame bytes in the frame		
		[07] STRM1  00 00 7E 00 
		[08] STRM2  00 00 1A 00 
		[09] STRM3  00 00 10 00 
		[10] STRM4  03 03 03 03  
		[11] STRM5  F1 F1 F1 F1 
		[12] STRM6  D1 D1 D0 D0
		[13] CHKSUM1 --|2 CRC bytes sent by RX (calculated on RX side crc16/table)
		[14] CHKSUM2 --|*/
		//len=17 -> len-7=10 -> 3..12
		uint16_t lcrc = FrSkyX_crc(&buffer[3], len-7 ) ;
		if ( ( (lcrc >> 8) != buffer[len-4]) || ( (lcrc & 0x00FF ) != buffer[len-3]) )
			return false;									// Bad CRC
		
		if(buffer[4] & 0x80)
			RX_RSSI=buffer[4] & 0x7F ;
		else
			v_lipo1 = (buffer[4]<<1) + 1 ;
		#if defined(TELEMETRY_FRSKYX_TO_FRSKYD) && defined(ENABLE_PPM)
			if(mode_select != MODE_SERIAL)
				return true;
		#endif
	}
	if (protocol==PROTO_FRSKYX||protocol==PROTO_FRSKYX2||protocol==PROTO_FRSKY_R9)
	{
		telemetry_lost=0;

		//Save outgoing telemetry sequence
		FrSkyX_TX_IN_Seq=buffer[5] >> 4;

		//Check incoming telemetry sequence
		uint8_t packet_seq=buffer[5] & 0x03;
		if ( buffer[5] & 0x08 )
		{//Request init
			FrSkyX_RX_Seq = 0x08 ;
			FrSkyX_RX_NextFrame = 0x00 ;
			FrSkyX_RX_Frames[0].valid = false ;
			FrSkyX_RX_Frames[1].valid = false ;
			FrSkyX_RX_Frames[2].valid = false ;
			FrSkyX_RX_Frames[3].valid = false ;
		}
		else if ( packet_seq == (FrSkyX_RX_Seq & 0x03 ) )
		{//In sequence
			struct t_FrSkyX_RX_Frame *p ;
			uint8_t count ;
			// buffer[4] RSSI
			// buffer[5] sequence control
			// buffer[6] payload count
			// buffer[7-12] payload			
			p = &FrSkyX_RX_Frames[packet_seq] ;
			count = buffer[6];					// Payload length
			if ( count <= 6 )
			{//Store payload
				p->count = count ;
				for ( uint8_t i = 0 ; i < count ; i++ )
					p->payload[i] = buffer[i+7] ;
			}
			else
				p->count = 0 ;					// Discard
			p->valid = true ;

			FrSkyX_RX_Seq = ( FrSkyX_RX_Seq + 1 ) & 0x03 ;	// Move to next sequence

			if ( FrSkyX_RX_ValidSeq & 0x80 )
			{
				FrSkyX_RX_Seq = ( FrSkyX_RX_ValidSeq + 1 ) & 3 ;
				FrSkyX_RX_ValidSeq &= 0x7F ;
			}

		}
		else
		{//Not in sequence
			struct t_FrSkyX_RX_Frame *q ;
			uint8_t count ;
			// buffer[4] RSSI
			// buffer[5] sequence control
			// buffer[6] payload count
			// buffer[7-12] payload			
			if ( packet_seq == ( ( FrSkyX_RX_Seq +1 ) & 3 ) )
			{//Received next sequence -> save it
				q = &FrSkyX_RX_Frames[packet_seq] ;
				count = buffer[6];				// Payload length
				if ( count <= 6 )
				{//Store payload
					q->count = count ;
					for ( uint8_t i = 0 ; i < count ; i++ )
						q->payload[i] = buffer[i+7] ;
				}
				else
					q->count = 0 ;
				q->valid = true ;
			
				FrSkyX_RX_ValidSeq = 0x80 | packet_seq ;
			}
			FrSkyX_RX_Seq = ( FrSkyX_RX_Seq & 0x03 ) | 0x04 ;	// Request re-transmission of original sequence
		}
	}
#endif
	return true;
}

void init_frskyd_link_telemetry()
{
	telemetry_link=0;
	telemetry_counter=0;
	telemetry_lost=1;
	v_lipo1=0;
	v_lipo2=0;
	RX_RSSI=0;
	TX_RSSI=0;
	RX_LQI=0;
	TX_LQI=0;
	#if defined HUB_TELEMETRY
		FrSkyD_User_Frame_Start=FrSkyD_User_Frame_End=0;
	#endif
}

void frsky_link_frame()
{
	frame[0] = 0xFE;			// Link frame
	if (protocol==PROTO_FRSKYD)
	{		
		frame[1] = telemetry_in_buffer[3];		// A1
		frame[2] = telemetry_in_buffer[4];		// A2
		frame[3] = telemetry_in_buffer[5];		// RX_RSSI
		telemetry_link &= ~1 ;		// Sent
		telemetry_link |= 2 ;		// Send hub if available
	}
	else
	{//PROTO_HUBSAN, PROTO_AFHDS2A, PROTO_BAYANG, PROTO_NCC1701, PROTO_CABELL, PROTO_HITEC, PROTO_BUGS, PROTO_BUGSMINI, PROTO_FRSKYX, PROTO_FRSKYX2, PROTO_PROPEL, PROTO_DEVO, PROTO_RLINK, PROTO_OMP, PROTO_WFLY2, PROTO_LOLI, PROTO_MLINK, PROTO_MT99XX
		frame[1] = v_lipo1;
		frame[2] = v_lipo2;
		frame[3] = RX_RSSI;
		telemetry_link &= ~1 ;		// Sent
	}
	frame[4] = TX_RSSI;
	frame[5] = RX_LQI;
	frame[6] = TX_LQI;
	frame[7] = frame[8] = 0;
	#if defined MULTI_TELEMETRY
		multi_send_frskyhub();
	#else
		frskySendStuffed();
	#endif
}

#if defined HUB_TELEMETRY
void frsky_user_frame()
{
	if(telemetry_in_buffer[6])
	{//only send valid hub frames
		frame[0] = 0xFD;				// user frame
		if(telemetry_in_buffer[6]>USER_MAX_BYTES)
		{
			frame[1]=USER_MAX_BYTES;	// packet size
			telemetry_in_buffer[6]-=USER_MAX_BYTES;
			telemetry_link |= 2 ;			// 2 packets need to be sent
		}
		else
		{
			frame[1]=telemetry_in_buffer[6];			// packet size
			telemetry_link &= ~2;			// only 1 packet or processing second packet
		}
		frame[2] = telemetry_in_buffer[7];
		for(uint8_t i=0;i<USER_MAX_BYTES;i++)
			frame[i+3]=telemetry_in_buffer[i+8];
		if(telemetry_link & 2)				// prepare the content of second packet
			for(uint8_t i=8;i<USER_MAX_BYTES+8;i++)
				telemetry_in_buffer[i]=telemetry_in_buffer[i+USER_MAX_BYTES];
		#if defined MULTI_TELEMETRY
			multi_send_frskyhub();
		#else
			frskySendStuffed();
		#endif
	}
	else
		telemetry_link &= ~2;
}
/*
HuB RX packets.
packet_in[6]|(counter++)|00 01 02 03 04 05 06 07 08 09 
        %32        
01     08          5E 28 12 00 5E 5E 3A 06 00 5E
0A     09          28 12 00 5E 5E 3A 06 00 5E 5E  
09     0A          3B 09 00 5E 5E 06 36 7D 5E 5E 
03     0B          5E 28 11 00 5E 5E 06 06 6C 5E
0A     0C          00 5E 5E 3A 06 00 5E 5E 3B 09
07     0D          00 5E 5E 06 06 6C 5E 16 72 5E
05     0E          5E 28 11 00 5E 5E 3A 06 00 5E
0A     0F          5E 3A 06 00 5E 5E 3B 09 00 5E
05     10          5E 06 16 72 5E 5E 3A 06 00 5E
*/
void frsky_write_user_frame(uint8_t ID, uint8_t low, uint8_t high)
{
	telemetry_in_buffer[6]  = 0x04;		// number of bytes in the payload
	telemetry_in_buffer[7]  = 0x00;		// unknown?
	telemetry_in_buffer[8]  = 0x5E;		// start of payload
	telemetry_in_buffer[9]  = ID;		// ID must be less than 0x40
	uint8_t pos=10;
	uint8_t value = low;
	for(uint8_t i=0;i<2;i++)
	{// Byte stuffing
		if(value == 0x5D || value == 0x5E)
		{// Byte stuffing
			telemetry_in_buffer[pos+1] = value ^ 0x60;
			telemetry_in_buffer[pos] = 0x5D;
			telemetry_in_buffer[6]++;	// 1 more byte in the payload
			pos += 2;
		}
		else
			telemetry_in_buffer[pos++] = value;
		value = high;
	}
	telemetry_link |= 2;				// request to send frame
}

void frsky_send_user_frame(uint8_t ID, uint8_t low, uint8_t high)
{
	if(telemetry_link&2)
	{ // add to buffer
		uint8_t test = (FrSkyD_User_Frame_End + 1) & 0x07;
		if(test == FrSkyD_User_Frame_Start)
			return;	// buffer full...
		FrSkyD_User_Frame_End = test;
		FrSkyD_User_Frame[FrSkyD_User_Frame_End].ID   = ID;
		FrSkyD_User_Frame[FrSkyD_User_Frame_End].low  = low;
		FrSkyD_User_Frame[FrSkyD_User_Frame_End].high = high;
	}
	else // send to TX direct
		frsky_write_user_frame(ID, low, high);
}

void frsky_check_user_frame()
{
	if((telemetry_link&2) || FrSkyD_User_Frame_Start == FrSkyD_User_Frame_End)
		return;		// need to wait that the last frame is sent or buffer is empty
	frsky_write_user_frame(FrSkyD_User_Frame[FrSkyD_User_Frame_Start].ID, FrSkyD_User_Frame[FrSkyD_User_Frame_Start].low, FrSkyD_User_Frame[FrSkyD_User_Frame_Start].high);
	FrSkyD_User_Frame_Start++;
	FrSkyD_User_Frame_Start &= 0x07;
}
#endif


#if defined SPORT_TELEMETRY
/* SPORT details serial
	100K 8E2 normal-multiprotocol
	-every 12ms-or multiple of 12; %36
	1  2  3  4  5  6  7  8  9  CRC DESCR
	7E 98 10 05 F1 20 23 0F 00 A6 SWR_ID 
	7E 98 10 01 F1 33 00 00 00 C9 RSSI_ID 
	7E 98 10 04 F1 58 00 00 00 A1 BATT_ID 
	7E BA 10 03 F1 E2 00 00 00 18 ADC2_ID 
	7E BA 10 03 F1 E2 00 00 00 18 ADC2_ID 
	7E BA 10 03 F1 E2 00 00 00 18 ADC2_ID 
	7E BA 10 03 F1 E2 00 00 00 18 ADC2_ID 
	7E BA 10 03 F1 E2 00 00 00 18 ADC2_ID 
	7E BA 10 03 F1 E2 00 00 00 18 ADC2_ID 	
	
	
	Telemetry frames(RF) SPORT info 
	15 bytes payload
	SPORT frame valid 6+3 bytes
	[00] PKLEN  0E 0E 0E 0E 
	[01] TXID1  DD DD DD DD 
	[02] TXID2  6D 6D 6D 6D 
	[03] CONST  02 02 02 02 
	[04] RS/RB  2C D0 2C CE	//D0;CE=2*RSSI;....2C = RX battery voltage(5V from Bec)
	[05] HD-SK  03 10 21 32	//TX/RX telemetry hand-shake bytes
	[06] NO.BT  00 00 06 03	//No.of valid SPORT frame bytes in the frame		
	[07] STRM1  00 00 7E 00 
	[08] STRM2  00 00 1A 00 
	[09] STRM3  00 00 10 00 
	[10] STRM4  03 03 03 03  
	[11] STRM5  F1 F1 F1 F1 
	[12] STRM6  D1 D1 D0 D0
	[13] CHKSUM1 --|2 CRC bytes sent by RX (calculated on RX side crc16/table)
	[14] CHKSUM2 --|
	+2	appended bytes automatically  RSSI and LQI/CRC bytes(len=0x0E+3);
	
0x06	0x06	0x06	0x06	0x06

0x7E	0x00	0x03	0x7E	0x00
0x1A	0x00	0xF1	0x1A	0x00
0x10	0x00	0xD7	0x10	0x00
0x03	0x7E	0x00	0x03	0x7E
0xF1	0x1A	0x00	0xF1	0x1A
0xD7	0x10	0x00	0xD7	0x10

0xE1	0x1C	0xD0	0xEE	0x33
0x34	0x0A	0xC3	0x56	0xF3
*/

#if defined MULTI_TELEMETRY
const uint8_t PROGMEM Indices[] = {	0x00, 0xA1, 0x22, 0x83, 0xE4, 0x45,
									0xC6, 0x67, 0x48, 0xE9, 0x6A, 0xCB,
									0xAC, 0x0D, 0x8E, 0x2F, 0xD0, 0x71,
									0xF2, 0x53, 0x34, 0x95, 0x16, 0xB7,
									0x98, 0x39, 0xBA, 0x1B } ;
#endif

#ifdef MULTI_TELEMETRY
	void sportSend(uint8_t *p)
	{
		multi_send_header(MULTI_TELEMETRY_SPORT, 9);
		uint16_t crc_s = 0;
		uint8_t x = p[0] ;
		if ( x <= 0x1B )
			x = pgm_read_byte_near( &Indices[x] ) ;
		Serial_write(x) ;
		for (uint8_t i = 1; i < 8; i++)
		{
			Serial_write(p[i]);
			crc_s += p[i];			//0-1FF
			crc_s += crc_s >> 8;	//0-100
			crc_s &= 0x00ff;
		}
		Serial_write(0xff - crc_s);
	}
#else
	void sportSend(uint8_t *p)
	{
		uint16_t crc_s = 0;
		Serial_write(START_STOP);//+9
		Serial_write(p[0]) ;
		for (uint8_t i = 1; i < 9; i++)
		{
			if (i == 8)
				p[i] = 0xff - crc_s;
			
			if ((p[i] == START_STOP) || (p[i] == BYTESTUFF))
			{
				Serial_write(BYTESTUFF);//stuff again
				Serial_write(STUFF_MASK ^ p[i]);
			} 
			else			
				Serial_write(p[i]);					
			
			crc_s += p[i]; //0-1FF
			crc_s += crc_s >> 8; //0-100
			crc_s &= 0x00ff;
		}
	}	
#endif

void sportIdle()
{
	#if !defined MULTI_TELEMETRY
		Serial_write(START_STOP);
	#endif
}	

void sportSendFrame()
{
	static uint8_t sport_counter=0;
	uint8_t i;

	sport_counter = (sport_counter + 1) %36;
	if(telemetry_lost)
	{
		sportIdle();
		return;
	}
	if(sport_counter<6)
	{
		frame[0] = 0x98;
		frame[1] = 0x10;
		for (i=5;i<8;i++)
			frame[i]=0;
	}
	switch (sport_counter)
	{
		case 0:
			frame[2] = 0x05;
			frame[3] = 0xf1;
			frame[4] = 0x02; //dummy values if swr 20230f00
			frame[5] = 0x23;
			frame[6] = 0x0F;
			break;
		case 2: // RSSI
			frame[2] = 0x01;
			frame[3] = 0xf1;
			frame[4] = RX_RSSI;
			frame[5] = TX_RSSI;
			frame[6] = RX_LQI;
			frame[7] = TX_LQI;
			break;
		case 4: //BATT
			frame[2] = 0x04;
			frame[3] = 0xf1;
			frame[4] = v_lipo1; //a1;
			break;								
		default:
			if(Sport_Data)
			{	
				for (i=0;i<FRSKY_SPORT_PACKET_SIZE;i++)
					frame[i]=pktx1[i];
				Sport_Data -- ;
				if ( Sport_Data )
				{
					uint8_t j = Sport_Data * FRSKY_SPORT_PACKET_SIZE ;
					for (i=0;i<j;i++)
						pktx1[i] = pktx1[i+FRSKY_SPORT_PACKET_SIZE] ;
				}
				break;
			}
			else
			{
				sportIdle();
				return;
			}		
	}
	sportSend(frame);
}	

void proces_sport_data(uint8_t data)
{
	static uint8_t pass = 0, indx = 0;
	switch (pass)
	{
		case 0:
			if (data == START_STOP)
			{//waiting for 0x7e
				indx = 0;
				pass = 1;
			}
			break;		
		case 1:
			if (data == START_STOP)	// Happens if missed packet
			{//waiting for 0x7e
				indx = 0;
				pass = 1;
				break;		
			}
			if(data == BYTESTUFF)	//if they are stuffed
				pass=2;
			else
				if (indx < MAX_PKTX)		
					pktx[indx++] = data;		
			break;
		case 2:	
			if (indx < MAX_PKTX)	
				pktx[indx++] = data ^ STUFF_MASK;	//unstuff bytes	
			pass=1;
			break;	
	} // end switch
	if (indx >= FRSKY_SPORT_PACKET_SIZE)
	{//8 bytes no crc 
		if ( Sport_Data < FX_BUFFERS )
		{
			uint8_t dest = Sport_Data * FRSKY_SPORT_PACKET_SIZE ;
			uint8_t i ;
			for ( i = 0 ; i < FRSKY_SPORT_PACKET_SIZE ; i++ )
				pktx1[dest++] = pktx[i] ;	// Triple buffer
			Sport_Data += 1 ;//ok to send
		}
//		else
//		{
//			// Overrun
//		}
		pass = 0;//reset
	}
}

#endif

void TelemetryUpdate()
{
	// mpm-v2: the status is the core's (mpm.c), room for a frame in the
	// telemetry buffer as the MPM's check
	if(mpm_port_write_free() < 48)
		return;
	#if defined SPORT_TELEMETRY
		if ((protocol==PROTO_FRSKYX || protocol==PROTO_FRSKYX2||protocol==PROTO_FRSKY_R9) && telemetry_link 
		#ifdef TELEMETRY_FRSKYX_TO_FRSKYD
			&& mode_select==MODE_SERIAL
		#endif
		)
		{	// FrSkyX
			for(;;)
			{ //Empty buffer
				struct t_FrSkyX_RX_Frame *p ;
				uint8_t count ;
				p = &FrSkyX_RX_Frames[FrSkyX_RX_NextFrame] ;
				if ( p->valid )
				{
					count = p->count ;
					for (uint8_t i=0; i < count ; i++)
						proces_sport_data(p->payload[i]) ;
					p->valid = false ;	// Sent
					FrSkyX_RX_NextFrame = ( FrSkyX_RX_NextFrame + 1 ) & 3 ;
				}
				else
					break ;
			}
			telemetry_link=0; 
			sportSendFrame();
		}
	#endif // SPORT_TELEMETRY

	#ifdef MULTI_TELEMETRY
		#if defined MULTI_CONFIG_INO
			if(telemetry_link && protocol == PROTO_CONFIG)
			{
				CONFIG_frame();
				telemetry_link=0;
				return;
			}
		#endif
		#if defined MLINK_FW_TELEMETRY
			if(telemetry_link && protocol == PROTO_MLINK)
			{
				MLINK_frame();
				telemetry_link=0;
				return;
			}
		#endif
		#if defined DSM_TELEMETRY
			if(telemetry_link && protocol == PROTO_DSM)
			{	// DSM
				DSM_frame();
				telemetry_link=0;
				return;
			}
		#endif
		#if defined AFHDS2A_FW_TELEMETRY
			if(telemetry_link == 2 && protocol == PROTO_AFHDS2A)
			{
				AFHDSA_short_frame();
				telemetry_link=0;
				return;
			}
		#endif
		#if defined HITEC_FW_TELEMETRY
			if(telemetry_link == 2 && protocol == PROTO_HITEC)
			{
				HITEC_short_frame();
				telemetry_link=0;
				return;
			}
		#endif
		#if defined HOTT_FW_TELEMETRY
			if(telemetry_link == 2 && protocol == PROTO_HOTT)
			{
				HOTT_short_frame();
				telemetry_link=0;
				return;
			}
		#endif
		#if defined RLINK_HUB_TELEMETRY
			if(telemetry_link == 2 && protocol == PROTO_RLINK)
			{
				RLINK_raw_frame();
				telemetry_link=0;
				return;
			}
		#endif
		#if defined SCANNER_TELEMETRY
			if (telemetry_link && protocol == PROTO_SCANNER)
			{
				spectrum_scanner_frame();
				telemetry_link = 0;
				return;
			}
		#endif
		#if defined (FRSKY_RX_TELEMETRY) || defined(AFHDS2A_RX_TELEMETRY) || defined (BAYANG_RX_TELEMETRY) || defined (DSM_RX_CYRF6936_INO)
			if ((telemetry_link & 1) && (protocol == PROTO_FRSKY_RX || protocol == PROTO_AFHDS2A_RX || protocol == PROTO_BAYANG_RX || protocol == PROTO_DSM_RX) )
			{
				receiver_channels_frame();
				telemetry_link &= ~1;
				return;
			}
		#endif
	#endif //MULTI_TELEMETRY
	
	if( telemetry_link & 1 )
	{	// FrSkyD + Hubsan + AFHDS2A + Bayang + Cabell + Hitec + Bugs + BugsMini + NCC1701 + PROPEL + RLINK + OMP + MLINK + DEVO
		// FrSkyX telemetry if in PPM
		frsky_link_frame();
		return;
	}
	#if defined HUB_TELEMETRY
		if((telemetry_link & 2) && ( protocol == PROTO_FRSKYD || protocol == PROTO_BAYANG || protocol == PROTO_MLINK  || protocol == PROTO_DEVO ) )
		{	// FrSkyD + Bayang + MLINK + DEVO
			frsky_user_frame();
			frsky_check_user_frame();
			return;
		}
	#endif
}


/**************************/
/**************************/
/**  Serial TX routines  **/
/**************************/

#endif // TELEMETRY
