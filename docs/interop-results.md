# Interoperability results

Over-the-air tests between the CC2652 (mpm-v2 `BENCH=ON`) and the chips of a
4-in-1 MULTI-Module (iRangeX IRX4 Lite with the bench firmware), side by side
on a desk, measured in October 2026. Setup and tools: [bench.md](bench.md).
What it means for the chip choice:
[cc2652-radio-assessment.md](cc2652-radio-assessment.md).

"MPM -> CC2652" means the MPM chip sends and the CC2652 receives. Unless noted,
a packet counts when all its bytes are right (a counter in the payload) and
its CRC is good on the receiving side. The percentages are from the
measurement campaign. The rig scripts reproduce them within a few percent
from run to run: re-runs gave 92 to 100% on some CC2652 -> MPM
directions (FrSky X, V8, Corona V2, Skyartec, SFHSS, Kyosho3) that had been
100%.

The sections below are the LaunchPad (CC2652R1, +5 dBm, no front end). The
same suites on the MPM v2 board are in [MPM v2](#mpm-v2).

## NRF24L01

`suite_nrf.py nrf1m nrf250k nrf2m` (250 packets, 16 byte payload):

| Direction | Received |
|---|---|
| CC2652 -> NRF, 1 Mbps / 2 Mbps | 99.6 to 100% / 92 to 97% |
| NRF -> CC2652, 1 Mbps and 250 kbps, addresses `e7e7e7e7e7`, `c3c3c3c3c3`, `6565656565`, `a1b2c3d4e5`, `0102030405` | 100% |
| NRF -> CC2652, `0f0f0f0f0f`, 1 Mbps / 250 kbps | 97.6 to 99.6% / 100% |
| NRF -> CC2652, `5555555555` (looks like the preamble everywhere) | ~2% |
| NRF -> CC2652, 2 Mbps | 0% (no 2 Mbps receiver) |

The RF core sync detection tolerates bit errors: a sync word that nearly
matches the bits sent before it triggers early and the packet is lost. With
the RX sync word on the last 4 bytes of address `e7e7e7e7e7` only 36% were
received at 1 Mbps (`c3c3c3c3c3`: 80%), and 100% with the NRF preamble and the
first 3 address bytes. The CC2652 doesn't need any lead-in before its sync word.
So the RX sync word is the 4 byte window of the known bytes (NRF preamble,
address, `prefix`) with the most bit errors against all the bits before it
(at least 8, the latest window then), see `frame.c`. When the best one has
fewer (e.g. `6565656565`: 6), a stricter correlation threshold is used: 0x32
instead of 0x1F gets it from 50% to 100%. Its sensitivity cost is not
measured. Detuning the CC2652 gives the same limit for both thresholds (100%
at 250 kHz, 0% at 350 kHz).

### Enhanced ShockBurst

Measured on air from the nRF24L01+:

- The packet control field is sent when EN_AA or dynamic payload length is
  enabled (EN_AA=0 with DYNPD too, e.g. FY326), not with EN_AA=0 and a static
  length (ARC doesn't matter).
- Its last bit is the datasheet's NO_ACK flag inverted: 1 = ack requested.
- With a static length, the length field is 0x33.
- The CRC covers the address, the 9 bit packet control field and the payload.
- Acks echo the packet id, length 0 or the ack payload length (static length:
  0x33), last bit 0. Acks of retransmissions (duplicates for the PRX) were
  seen with packet id 3: the CC2652 PTX, as the nRF24L01+, doesn't check it.

`esb.py` (CC2652 PTX with `ack`, NRF PRX with EN_AA, 250 packets, 16 byte
payload, address `a1b2c3d4e5`):

| Mode | Acked on the 1st try | NRF received |
|---|---|---|
| 250 kbps / 1 Mbps, dynamic or static length | 100% | 100% |
| 1 Mbps, 150 us ack window | 100% | 100% |
| 2 Mbps | 0% (2 Mbps RX) | 100% |
| NRF DPL packets (EN_AA=0) -> CC2652 `esb 1 1`, 250 kbps / 1 Mbps | | 100% |
| Ack payloads, 4 / 32 bytes, 250 kbps / 1 Mbps | 100% with payload (32 bytes at 250 kbps: `--window 1500`) | 100% |

The ack window must cover the ack with its payload, as the NRF's ARD: 1500 us
for 32 bytes at 250 kbps (500 us: 24% retransmissions).

## XN297

`suite_nrf.py xn297...` (250 packets, 16 byte payload, each direction):

| Mode | MPM -> CC2652 | CC2652 -> MPM |
|---|---|---|
| 1 Mbps (NRF), scrambled, CRC, 5 byte address | 100% | 100% |
| 250 kbps (CC2500 and NRF `force_nrf`) | 100% | 100% |
| Unscrambled 3 byte address / no CRC 4 byte address | 100% | 99.2 to 100% |
| Enhanced mode, scrambled, 1 Mbps and 250 kbps | 100% | 100% |
| Enhanced mode, unscrambled, 1 Mbps and 250 kbps | 100% (MPM branch `xn297-enhanced-xorout`) | 100% |

XN297 needs no prefix (3 byte preamble). The unscrambled enhanced mode CRC
xorout table (`xn297_crc_xorout_enhanced`, from the XN297 dump) is applied
only by `XN297_ReadEnhancedPayload()` in debug builds of the MPM: the branch
`xn297-enhanced-xorout` applies it on TX and RX in all builds. No MPM protocol
uses this mode, and it is checked against the CC2652 only, not against a real
XN297.

## CC2500

`suite_cc2500.py` (MPM registers of each protocol, CC2652 preset of the same
name; MPM receiving with `RXOFF_MODE`=IDLE and `rx 0`, CRC_OK checked when the
CC2500 has `CRC_EN`; 200 to 1000 packets each way):

| PHY | MPM -> CC2652 | CC2652 -> MPM |
|---|---|---|
| FrSky D8 (31 kbps GFSK, CRC, 16 bit sync), ch 0 and 234 | 100% | 99.6 to 100% with `trim -35` (without: 90 to 100%) |
| FrSky X (D16 v1, 70 kbps GFSK, 32 bit sync) | 100% | 97 to 100% |
| FrSky X2 (D16 v2, 77 kbps, CRC) | 100% | 100% |
| FrSky X LBT (100 kbps 2-FSK, 2405 MHz base) | 100% | 100% with `trim -35` (without: 92.5%) |
| FrSky X2 LBT (CRC) | 100% | 100% |
| HoTT (250 kbps MSK, CRC, whitening, FEC), CHANNR 0 and 225 | 100% | 100% |
| HoTT telemetry format (sync 2c6e, 22 bytes) | 100% | 100% |
| Hitec (250 kbps MSK 2/8 step, 24 byte preamble, whitening), bind and ID sync, ch 0 and 140 | 100% | 100% |
| Redpine fast (250 kbps MSK 1/8 step), ch 0, 200, 247 and 255 (2502.2 MHz) | 100% | 100% |
| Redpine slow (10 kbps 2-FSK) | 100% | 100% |
| Skyartec (229 kbps MSK 1/8 step), bind and ID sync | 100% | 92 to 100% |
| IKEA Ansluta (250 kbps MSK 2/8 step, FEC with a variable length) | 100% | 98 to 100% |
| FrSky V8 (27 kbps GFSK) | 100% | 98 to 100% |
| Corona V1, V2, Flydream V3 (100 kbps 2-FSK) | 100% | 93 to 100% (V2: CC2652 power 0x10c3, see below) |
| Ares (50 kbps 2-FSK, sync 5a60) | 100% | 100% |
| RadioLink, DumboRC, RC4G (38 kbps GFSK, whitening) | 100% | 99.6 to 100% |
| Futaba SFHSS (128 kbps 2-FSK, CC2400 CRC) | 100% | 91 to 100% |
| FrSky L (below) | 100% | bit exact in chunks |

These two boards are about 35 kHz apart (15 ppm): the CC2500 receives the
CC2652 FrSky D packets from about -60 to 0 kHz of CC2652 detuning. The narrow
FrSky D8 filter (135 kHz) is the most sensitive, which is why the MPM has the
`option` frequency fine tuning. The CC2652 (175 kHz filter for D8) receives the
CC2500 without correction.

MSK with a partial phase step (`DEVIATN` M below 7, Hitec 2/8, Redpine,
Skyartec 1/8) is sent as 2-FSK too. The CC2500 wants more deviation than true
MSK (Hitec: 50 kHz 71%, 62.5 kHz 99.5%, 75 to 160 kHz 100%), the CC2652
receiver less than about 0.4 x rate (Skyartec: 100 kHz 0%, 57 to 85 kHz 100%)
and a narrower filter (350 kHz instead of 530): the presets use 0.32 x rate.

HoTT, from a raw capture of the CC2500 packets (bit exact): CRC over the
payload, PN9 whitening of payload and CRC, 1 or 2 trellis terminator bytes
0x0B (not whitened) to an even length, rate 1/2 convolutional code and 4x4
interleaving as in TI DN504. CC2500 MSK (`DEVIATN` 0x47: 8/8 phase step) is
2-FSK with a 0.5 modulation index, all bits inverted: the CC2652 syncs on
`5555 2c6e 2c6e` for the sync word `d391d391`.

The CC2500 `CC2400_EN` CRC (SFHSS), measured: CRC-16 0x8005 from 0 over all
bytes but the last 2, which are XORed into it unprocessed. FEC works with a
variable length (IKEA, unlike what the datasheet says): only length + 1 bytes
and the CRC are encoded, plus the terminator.

Corona V2 freezes the CC2500 AGC (`AGCCTRL0` 0xDC with `AGCCTRL2` 0x67 and
`AGCCTRL1` 0xFB): at about -18 dBm on the bench the CC2500 receives nothing
(not a CC2652 issue: lower the CC2652 power, `pwr 0x10c3` then `preset`).

### FrSky L

`FrSkyL_cc2500.ino` (CC2500, TX only), with the real protocol:

- On air: the CC2500 (`FRSKYL_cc2500_conf`: 2-FSK, `MDMCFG2` 0x00: no sync
  word, infinite length) sends a 256 byte chip stream at 125 kbaud, 47.6 kHz
  deviation: 6 bytes 0xAA and the 17 byte packet, each bit LSB first as 11
  chips (`0x0ED` for 0, `0x712` for 1), then `55 55 55`. No preamble was found
  before it (neither 0x55 nor 0xAA). The CC2652 `frskyl` framing is bit exact
  with `FrSkyL_encode_packet()` (unit test) and corrects up to 5 chip errors
  per bit.
- MPM -> CC2652 (`proto_capture.py frskyl`, packet CRC checked on the host):
  bind packets on ch 0, 114/114 in 2 s (18 ms period); data packets on one
  hop channel 100%, no trim. The CC2652 also got the packets of the channel 48
  MHz away (its crystal frequency) about 80 dB weaker (-95 to -105 dBm for
  -20 dBm), a spurious response.
- CC2652 -> MPM (`frskyl_reverse.py`): the CC2500 has no way to receive this
  stream in full (64 byte FIFO, the bench reads it when the packet is
  complete), so it received 60 byte chunks after 16 bit sync words taken from
  the stream, syncing again after each chunk: 50/50 chunks bit exact at every
  position, all the payload chips covered but 18 of the constant first byte
  (with several sync words), from -70 to +35 kHz `trim`.

## A7105

`suite_a7105.py` (500 packets each way, 5 ms period, ch 170, all bytes
compared, MPM CRC from the `MODE` register):

| PHY | MPM -> CC2652 | CC2652 -> MPM |
|---|---|---|
| FlySky (500 kbps, 21 bytes, CRC), ch 100, 160, 170 | 100% | 99.0 to 99.8% (CRC errors) |
| AFHDS2A (500 kbps, 38 bytes, CRC, FEC), ch 100, 160, 170 | 100% | 98.8 to 100% |
| Hubsan (100 kbps, 16 bytes, no CRC), ch 20, 100, 160, 170 | 100% | 100% |
| FlySky / AFHDS2A at ch 20 (2410 MHz, in Wi-Fi channel 1) | 100% | 92 / 96% |

The other A7105 protocols, with their `A7105_Init()` tables (MPM `preset
<name>`, `w 0x06 <id>`) against the CC2652 presets of the same name. Most only
transmit and their tables don't receive: for CC2652 -> MPM the MPM uses the
AFHDS2A table set to the protocol's `DATA_RATE` (0x0E), `CODE_I` (0x1F), FIFO
length (0x03) and ID. `CODE_I` bit 3 is the CRC, bit 4 the FEC; `DATA_RATE` 0
= 500 kbps, 1 = 250 kbps, 3 = 125 kbps, 4 = 100 kbps.

| Protocol (rate, bytes, CRC / FEC, ID) | MPM -> CC2652 | CC2652 -> MPM |
|---|---|---|
| Bugs (250 kbps, 22, CRC, 5475c52a) | 100% | 99% (AFHDS2A table) |
| Height 5 ch / 8 ch (250 kbps, 8 / 11, CRC + FEC, 25a53c45) | 100% | 88 to 100% / 100% |
| Pelikan (250 kbps, 16, none, 06230623) | 100% | 99.5 to 99.8% |
| Kyosho FHSS (500 kbps, 38, CRC + FEC) | 100% | 99.6 to 100% |
| Kyosho Hype (250 kbps, 17, CRC + FEC) | 100% | 99 to 100% |
| WFLY2 (500 kbps, 32, CRC) | 100% | 96.5 to 97.8% (CRC errors) |
| Joysway (500 kbps, 16, CRC) | 100% | 98.5 to 98.6% (CRC errors) |

- Bugs receives telemetry at `PLL_I` - 2 with its own table (as
  `Bugs_a7105.ino`); on the bench it then receives nothing.
- Height 5 ch: the missing packets come without CRC errors, 8 byte packets
  every 5 ms. Height 8 ch (11 bytes) gets 100%: more likely the A7105 RX
  restart between short packets than the format.
- Pelikan also works at 125 kbps (`DATA_RATE` 3).

All with `trim 90`: this A7105 is about 90 kHz above the CC2652 (37 ppm,
measured with a narrow CC2652 filter on Hubsan: 100% from +60 to +120 kHz, 0%
at 0 and +140). Without it the CC2652 still receives the A7105 (1.4 MHz
filter), but the A7105 loses 25 to 45% at 500 kbps, more at -50 kHz.

On air (measured on raw captures, bit exact): preamble, the ID as sync word,
the fixed length payload, MSB first. CRC-16 CCITT (0x1021) from 0x1D0F over
the payload only. FEC: each nibble (high first) as a Hamming (7,4) code word,
data bits first (0 -> 0x00, 1 -> 0x0B, ... f -> 0x7F), 14 bits per byte, padded
to a byte at the end. The A7105 receives best with a 250 kHz deviation from the
CC2652 (`TX_II` 0x2B, FlySky without FEC: 186 kHz 2.4% CRC errors, 250 kHz
0.6%, 140 and 300 kHz worse). The remaining CRC errors at 500 kbps do not
depend on the CC2652 power: no second A7105 to compare with.

## CYRF6936

`suite_cyrf.py` (100 packets each way, all bytes compared, 10 to 20 ms
period):

| Packets | MPM -> CC2652 | CC2652 -> MPM |
|---|---|---|
| DSM data (8DR, SOP, length, CRC), 10 and 16 bytes, ch 0, 30, 70, 79 | 100% | 100% |
| DSM data, other SOP / data codes, CRC seed 0xa55a (`cyrf`) | 100% | 100% |
| DSM bind (SDR, 16 bytes), first data bit 0 and 1 | 100% | 100% |
| DSM bind response (SDR, 10 bytes), with and without `prefix` | 99 to 100% | 100% |
| Devo (32 chip SOP and data codes, length, CRC, 32 chip preamble) | 100% | 100% |
| Devo SOP codes 1 and 2, CRC seed 0xa55a | 100% | 100% |
| 64 chip SOP, 32 chip data (WFLY: `w 0x10 0xE4`) | 100% | 100% |
| 32 chip SOP, 64 chip data (Kyosho3: `w 0x03 0x28`) | 100% | 100% |

The other CYRF6936 protocols, with their own register setups (the bind
configuration of each `*_cyrf6936.ino`, written with `w`), ch 30:

| Protocol (format) | MPM -> CC2652 | CC2652 -> MPM |
|---|---|---|
| J6Pro (64 chip 8DR, SOP, length, CRC) | 100% | 100% |
| WK2x01 (64 chip 8DR, SOP, length, CRC) | 100% | 100% |
| Scorpio (64 chip 8DR, default SOP, CRC seed 1) | 100% | 97 to 100% |
| Kyosho3 (32 chip SOP, 64 chip data, no CRC, TX_INV) | 100% | 95 to 99% |
| Traxxas (32 chip 8DR, CRC seed 0x5a5a) | 100% | 100% |
| WFLY (64 chip SOP, 32 chip data) | 99% | 100% |
| MLINK (64 chip SDR, no SOP, length or CRC) | 100% | 97 to 100% |
| Losi (64 chip SDR, as DSM bind) | 100% | 100% |

No `trim` needed: the CYRF6936 still receives with the CC2652 250 kHz off, not
at 500 kHz. DSM telemetry uses the data packet format.

Measured along the way:

- Frequencies: the CYRF6936 sends at 2400 + n MHz, 2401 + n with `TX_OFFSET`
  0x0555 (set by most protocols, not Kyosho3, MLINK or the GFSK mode). It
  receives at about 2401.2 + n with `RX_CFG` FAST_TURN_EN (bit 3) and bit 4
  clear (0x4A), at 2402 + n otherwise (0x52, 0x42, 0x5A, 0x12). On the image
  frequency it receives inverted data.
- `TX_OVERRIDE` TX_INV (Kyosho3) inverts every chip, the preamble included.
- The CYRF6936 default SOP code is `82c79036219eff17` (Scorpio, Kyosho3: 32
  chip, the first 4 bytes). J6Pro's `j6pro_data_code` is the default data code
  in reversed byte order, despite its comment.
- MLINK sets `RX_CFG` VLD_EN (bit 0): each received byte is then followed by a
  valid flag byte, while the bench reads `RX_COUNT` bytes. The reverse run turns
  it off. It also needed the CC2652 to send inverted chips, which isn't
  understood: MLINK's own packets aren't inverted.

On air (measured on raw captures, bit exact), at 1 Mchip/s, as received by
the CC2652 (one bit per chip). Codes are sent LSB first, as given to
`CYRF_ConfigSOPCode()` / `CYRF_ConfigDataCode()`:

- Preamble: `1100` repeated, 16 chips per count (`CYRF_WritePreamble()`,
  `0x24`: count, then the pattern; DSM: 4, the default and Devo: 2).
- With SOP: the SOP code (64 chips, or its first 4 bytes with
  `FRAMING_CFG.SOP_LEN` 0), 4 alternating chips starting with the last SOP
  chip inverted (`0101` or `1010`), the SOP code again, inverted with 64 chip
  data codes only.
- One gap chip, the last chip inverted, then the data. 8DR, 64 chip data
  codes: one symbol per byte, the data code (or the second one, `data_code +
  8`, if bit 6 is set) rotated by `v & 63` chips, inverted unless bit 7 is
  set. 32 chip data codes (`TX_CFG.DATA_CODE_LENGTH` 0): the 16 data code
  bytes are 4 codes (bits 5-6), rotated by `v & 31`. SDR: one code per bit,
  LSB first: the data code for a 1, inverted for a 0.
- Devo doesn't set the data codes: the CYRF6936 default is
  `6fbe3201dbf12b01e35cfa029793f902`.

The CC2652 RX sync word: in the SOP part with a SOP (on the preamble most
packets were missed). With 32 chip data codes both SOP copies are the same: a
sync word on the second one also matches the first, 36 chips earlier, and no
packet decoded. So the sync word choice compares each window with all the
bits before it, not only the previous 32: here it spans the separator. Without
a SOP only the preamble is known before the data: a sync word on it gets 25 to
35% of the bind packets at any threshold. The RX sync word is then the last 8
preamble chips, the gap and the first 23 chips of the first data bit, with the
alternative sync word of `CMD_PROP_RX_ADV` for the other bit value, and the
strict threshold (0x1F: 74% / 45% of the bind responses). With a `prefix`
(bind response: its first 4 bytes are the inverted manufacturer id of the TX),
the sync word is in the data: 100% at 0x1F.

### GFSK mode (E01X, E129, E010R5)

`CYRF_GFSK1M_Init()`, with the real protocols (`proto_capture.py`: bind
packets on the bind channel, E015 also data packets):

| Protocol (framing) | Packets received | CRC right |
|---|---|---|
| E012 bind (HS6200, 15 bytes, ch 60) | all | 3 to 4% (MPM bug, see below) |
| E015 bind / data (HS6200, 9 / 10 bytes, ch 45) | all | 95 to 100% / 0% |
| E129 / C186 bind (RF2500, scrambled, 16 / 19 bytes, ch 45) | all | 100% |
| E010R5 bind (RF2500, 14 bytes, ch 48) | all | 83%: 1 in 6 corrupted (MPM bug); 100% on ch 48 and 56 with PR #1197 |

- The GFSK mode sends bytes LSB first (the MPM bit-reverses its buffers), at
  2400 + n MHz, after a 0xAA preamble on air (about 2 bytes with the E01X
  count of 1), whatever the first address bit.
- HS6200 CRC: `HS6200_calc_crc()` takes the last CRC bit from the byte after
  the payload, `msg[pos+1]` instead of `msg[pos]`. That byte isn't written yet:
  it holds stack contents, and `crc16_update(x, 1)` mixes all 8 bits of it
  into the CRC. Each MPM CRC captured matches exactly one value of that byte,
  and the value changes from packet to packet. The payloads are all right. The
  CC2652 sends the CRC as intended. Without an E01X model nobody can tell what
  the models check.
  Potential fix, not applied (it needs someone with an E01X model to test):
  `crc16_update(msg[pos], 1);` in `HS6200_calc_crc()`. After the loop
  `msg[pos]` is the byte holding the last payload bit in bit 7, its other bits
  are 0. That is the CRC the CC2652 sends, bit exact with this fix (unit test).
  If the models turn out to accept the current CRCs, they don't check it and
  the fix is harmless.
- E010R5: the callback changes the channel right after `RF2500_SendPayload()`,
  while the CYRF6936 still sends the end of the packet
  (`CYRF_GFSK1M_SendPayload()` returns once its FIFO is filled). The last
  packet before each hop was corrupted, every time. Fixed in
  [PR #1197](https://github.com/pascallanger/DIY-Multiprotocol-TX-Module/pull/1197):
  the hop first waits for `TX_IRQ_STATUS` TXC (bounded loop), then 100% of
  the packets are right, same timing.

All the MPM protocols of the four chips have been run: the CC2500, A7105 and
CYRF6936 DSSS ones with their own register setups, the GFSK mode ones and
FrSky L with the real protocol code. CFlie at 2 Mbps is the only one left
(no 2 Mbps receiver on the CC2652).

## MPM v2

The same suites with the MPM v2 board (CC2652RB, SKY66112 PA/LNA, see
[hardware.md](hardware.md)) in place of the LaunchPad: `CC_PORT=v2`, the two
modules about 1 m apart on a desk, MPM v2 at its default power (`0x2853`,
0 dBm into the PA, about +20 dBm out), October 2026.

**Frequency.** The CC2652RB runs on its BAW oscillator (HPOSC), about 98 ppm
fast on this chip (`info`: `hposc=-411`). With the HPOSC correction in
`radio.c` it lands within about 10 kHz of the LaunchPad: the CC2500 receives
the MPM v2 FrSky D packets from -80 to 0 kHz of `trim` (LaunchPad: -60 to 0),
the A7105 Hubsan packets reach the MPM v2 from about -100 to +300 kHz of
detuning (centre about +100, LaunchPad +90). The `trim` values of the setups
carry over.

| Family | MPM -> MPM v2 | MPM v2 -> MPM |
|---|---|---|
| NRF24L01 1 Mbps / 250 kbps, 6 addresses | 99.6 to 100% | 99.2 to 100% |
| NRF24L01 2 Mbps | (no 2 Mbps RX) | 94.8 to 100% |
| XN297, all modes of `suite_nrf.py` | 100% | 100% |
| ESB 250 kbps (`esb.py`) | | 250/250 acked on the 1st try |
| CC2500, the 18 setups of `suite_cc2500.py` | 100% | 80 to 100%, see below |
| A7105, the 11 setups of `suite_a7105.py` | 100% | 94.4 to 100% (Joysway 94.4%, Bugs 97.6%, WFLY2 97.8%) |
| CYRF6936, the 12 setups of `suite_cyrf.py` | 96 to 100% (LaunchPad: 99 to 100%), Wi-Fi, see below | 99 to 100% |

CC2500, MPM v2 -> MPM below 100% at full power: Redpine 80%, Hitec 93%,
Ares and FrSky X LBT 94%, Redpine slow 96.5%, FrSky X, IKEA and Corona V2 97%.
Three causes, none on the MPM v2 side:

- **Too much signal.** At -5 dBm (`CC_INIT="pwr 0x12d6"`) Hitec and Ares get
  100%; at -21 dBm (`0x04c6`) they drop again (90 to 96.5%). +20 dBm at 1 m
  overloads the 4-in-1 receiver (it has an LNA too).
- **MPM read stalls.** Redpine (3 ms period) loses runs of exactly 10 packets
  in some runs: the packet before the gap is read 33 ms late by the MPM, while
  the MPM v2 sends on time (`stop` reports `late_max`, under 10 us). With the
  CC2500 going idle after each packet ("rx 0"), everything sent during the
  stall is lost. Most runs get 100%.
- **MPM bench race (FrSky X LBT).** The "other" packets are the first 10 of a
  run, each 3 bytes short (last payload byte and status missing).
  `bench_receive()` reads `RXBYTES` before checking `MARCSTATE`: when the
  packet ends between the two reads, the MPM takes the radio as idle with the
  old byte count and flushes the rest. Fixed in `mpm-bench` (941479a, reads
  `MARCSTATE` first): `frskyxeu` rev 200/200, other=0, in 3 runs out of 3
  (`CC_INIT="pwr 0x12d6"`).

CYRF6936 -> MPM v2 losses (1 to 4% on DSM bind, Devo, WK2x01; LaunchPad: 99 to
100%): Wi-Fi, not overload. These setups are bind configurations: `TX_CFG`
PA 0 (`CYRF_BIND_POWER`), so about -50 dBm at the MPM v2. DSM data sends at
PA 7 (-18 dBm) and gets 100%.

- Power: Devo with PA 7 (`w 0x03 0x0f`) 200/200 twice; DSM data with PA 0
  (`w 0x03 0x28`) 97 to 98%. No RF core errors (`rf_nok=0`, `rf_buf_full=0`):
  the packets are missing (SOP not found) or have bytes wrong. The decoder
  takes the nearest code, so each wrong byte is a whole 32/64 chip symbol
  trashed: a burst, not a weak signal (-50 dBm is 40 dB above sensitivity).
- Channel (Devo PA 0, 250 packets): 83 to 93% at 2400 to 2418 MHz (7 Wi-Fi
  networks on channel 1 here), 96 to 100% elsewhere. No clean channel: the
  suite at ch 49 still gets 95 to 100%.
- Timing (Devo, ch 8, 10 ms period): 60 to 75% of the lost packets are 10 or
  11 packets apart (or multiples): 102.4 ms, the Wi-Fi beacon interval. The
  rest come in bursts (traffic).
- The CYRF6936 does the same: MPM v2 -> MPM at the lowest power
  (`CC_INIT="pwr 0x04c6"`, ch 8) Devo 90 to 92%, DSM 82 to 84%; at the default
  power 250/250.

The LaunchPad numbers were taken another day (other Wi-Fi traffic, 100
packets per setup). In real use only binding runs at PA 0, at short range.
