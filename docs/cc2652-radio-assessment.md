# CC2652 radio assessment

What the CC2652R RF core can and cannot do for the MULTI-Module protocols,
measured over the air against a 4-in-1 module, as input for the chip choice.

Measured 2026-10-18: CC2652R1 LaunchPad (PCB antenna) next to an iRangeX IRX4
Lite running the MPM bench firmware, 200 to 250 packets per test, signal around
-24 dBm. Test rig: [bench.md](bench.md), per-test results:
[interop-results.md](interop-results.md), status and next steps:
[status.md](status.md). CC2652 code: `src/radio.c`
(RF core driver) and `src/frame.c` (NRF24L01 / XN297 / CC2500 / A7105 /
CYRF6936 / HS6200 / RF2500 / FrSky L framing), built with `-DBENCH=ON`.

The same suites later ran on the MPM v2 prototype (CC2652RB with a SKY66112
PA/LNA) with the same results: [interop-results.md](interop-results.md#mpm-v2).
Its HPOSC oscillator is corrected from TI's factory calibration, so the board
itself needs no frequency calibration; the trims below are for the 4-in-1
chips' crystals and stay needed, as the MPM `option` is today.

## Summary

- Works: NRF24L01 and XN297 protocols at 250 kbps and 1 Mbps, both
  directions, including Enhanced ShockBurst auto-ack with retransmissions and
  ack payloads. Bare-metal, with two TI files (`rfc.c`/`rfc.h` and the
  proprietary CPE patch).
- Hard limit: no 2 Mbps reception. CFlie models set to 2 Mbps would not work.
- Works: every MPM CC2500 protocol tried, both directions: FrSky D8/V8/D16
  (FCC and LBT), HoTT, Hitec, Redpine, Skyartec, Corona, Flydream, Ares,
  RadioLink, IKEA, SFHSS, FrSky L (a 125 kbaud chip stream). That covers
  2-FSK, GFSK and MSK from 10 to 250 kbps, whitening, FEC and the CC2400 CRC,
  with a per-board frequency trim like the MPM `option`. FrSky
  itself runs ACCST and ACCESS on CC26xx chips in its receivers and transmitter
  modules (information from the project owner, not a bench result).
- Works: every MPM A7105 protocol: FlySky, AFHDS2A (FEC), Hubsan, Bugs,
  Height, Pelikan, Kyosho (FHSS and Hype), WFLY2, Joysway. 100% from the A7105,
  88 to 100% to it (A7105 CRC errors at 500 kbps), with a 90 kHz trim on this
  board.
- Works: every MPM CYRF6936 DSSS protocol, with its own register setup: DSM
  (data, bind and bind response packets), Devo, J6Pro, WK2x01, Scorpio,
  Kyosho3, Traxxas, WFLY, MLINK, Losi. 97 to 100% both ways without trim. The
  chip codes are sent and correlated in software; the 8.2 ms DSM bind packet
  (1029 bytes on air) needs the advanced RF core commands, above 255 bytes.
- Works: the CYRF6936 GFSK mode protocols (E01X, E129, E010R5): plain 1 Mbps
  GFSK, with the HS6200 and RF2500 framings the MPM builds in software. 100%
  of the MPM packets received. The MPM sends wrong HS6200 CRCs (an MPM bug,
  see below); the CC2652 sends the intended ones.
- Mitigated in software: repeated address patterns. Degenerate addresses such
  as `5555555555` still fail.
- Biggest unknown: receiver sensitivity (sync thresholds raised for weak sync
  words, DSSS processing gain lost: see below).

## Measured status

| Capability | Status | Result |
|---|---|---|
| NRF24L01 TX, 250 kbps / 1 Mbps / 2 Mbps | works | 100% / 99.6% / 96.5% |
| NRF24L01 RX, 250 kbps and 1 Mbps | works | 100% with ordinary addresses |
| NRF24L01 RX, repeated addresses, 1 Mbps | mitigated | `e7..`, `c3..`, `65..`: 100%, `0f..`: 97.6%, `55..`: ~2% |
| NRF24L01 RX, 2 Mbps | not possible | 0% |
| XN297 standard and enhanced, 250 kbps / 1 Mbps | works | 100% both ways |
| ESB transmitter with auto-ack (YD717, Propel, CFlie) | works | 100% acked on the 1st try, 250 kbps and 1 Mbps |
| ESB ack payloads, 4 and 32 bytes (Propel, CFlie telemetry) | works | 100% |
| TX to RX turnaround for acks | works | a 150 us ack window is enough at 1 Mbps |
| CC2500 FrSky D8 (31 kbps GFSK, CRC, 16 bit sync) | works | 100% both ways, with a 35 kHz trim for CC2652 -> CC2500 |
| CC2500 FrSky X / X2 (70 / 77 kbps GFSK), X / X2 LBT (100 kbps 2-FSK) | works | 100% both ways |
| CC2500 HoTT (250 kbps MSK, whitening, FEC), data and telemetry formats | works | 100% both ways, no trim needed |
| CC2500 MSK with a partial phase step: Hitec, Redpine fast, Skyartec, IKEA | works | 100% both ways with 0.32 x rate deviation |
| CC2500 2-FSK / GFSK: FrSky V8, Corona, Flydream, Ares, RadioLink, SFHSS, Redpine slow | works | 99.6 to 100% both ways |
| CC2500 FrSky L (125 kbaud 2-FSK chip stream, 256 bytes) | works | 100% CC2500 -> CC2652; CC2652 -> CC2500 bit exact in 60 byte chunks (the CC2500 can't receive the whole stream) |
| A7105 FlySky, AFHDS2A (500 kbps, CRC, FEC), Hubsan (100 kbps) | works | 100% A7105 -> CC2652, 99 to 99.8% (500 kbps) and 100% (Hubsan) CC2652 -> A7105, with a 90 kHz trim |
| Other A7105 protocols (Bugs, Height, Pelikan, Kyosho, Kyosho Hype, WFLY2, Joysway) | works | 100% A7105 -> CC2652, 88 to 100% CC2652 -> A7105 (see A7105 section) |
| CYRF6936 DSM data packets (8DR, SOP, length, CRC), 10 and 16 bytes, 4 channels | works | 100% both ways |
| CYRF6936 DSM bind and bind response (SDR, no SOP) | works | 100% both ways (bind TX -> CC2652 RX needs a sync word on the preamble end, see below) |
| CYRF6936 Devo (32 chip SOP and data codes, 32 chip preamble) | works | 100% both ways, 3 SOP codes, CRC seeds 0 and 0xa55a |
| CYRF6936 64 chip SOP + 32 chip data (WFLY), 32 chip SOP + 64 chip data (Kyosho3) | works | 100% both ways (Devo register setup with the WFLY / Kyosho3 framing) |
| Other CYRF6936 protocols with their registers (J6Pro, WK2x01, Scorpio, Kyosho3, Traxxas, WFLY, MLINK, Losi) | works | 99 to 100% CYRF6936 -> CC2652, 97 to 100% CC2652 -> CYRF6936 |
| CYRF6936 GFSK mode: E01X (HS6200 framing), E129 and E010R5 (RF2500 framing) | works | 100% of the MPM packets received (TX only protocols) |
| Sensitivity, range, blocking | not measured | no attenuator on the bench |

## Weaknesses, by impact

### 1. No 2 Mbps reception (blocks CFlie at 2 Mbps)

- What: the proprietary mode modem detects nothing at 2 Msps. Six
  combinations of RX bandwidth (up to the maximum) and deviation (250 to 500
  kHz) gave zero sync detections, not even false ones. TI's radio
  configuration database for the CC2652R has no 2.4 GHz proprietary setting
  above 250 kbps. 2 Mbps only exists as the BLE 2M PHY.
- Impact: 2 Mbps TX works (96.5%), so ESky150 (2 Mbps, TX only) is fine.
  CFlie selects 2 Mbps for some models, and its bind and data steps wait for
  ACKs (`packet_ack() == PKT_ACKED`), so those models would not work.
- Way out, untested: receive with the BLE 2M PHY. It needs TI's larger
  multi-protocol CPE patch, the modulation index differs (BLE 0.5, nRF24L01
  about 0.32), and the BLE receiver expects its access address right after the
  preamble and reads a length from the packet.

### 2. Sync detection fooled by repeated bit patterns (mitigated)

- What: the RF core sync correlator tolerates bit errors. With a repeated
  address, the sync word nearly matches bits sent earlier in the packet,
  triggers early and the packet is lost (nothing is received, not even with a
  bad CRC). Before the fix, at 1 Mbps: `e7e7e7e7e7` (the nRF24L01 default) 36%,
  `c3c3c3c3c3` 80%. The CC2652 doesn't need any lead-in before its sync word:
  syncing on the NRF preamble and the first 3 address bytes gave 100%.
- Mitigation: the RX sync word is the 32 bit window of the known bytes (NRF
  preamble, address, known payload prefix) with the most bit errors against
  the bits before it. When even the best one is weak, the correlation
  threshold is raised (register `0x5114`, 0x1F to 0x32). Result: 100% for
  `e7..`, `c3..`, `65..`, 97.6% for `0f0f0f0f0f`.
- Residual: addresses that look like the preamble everywhere (`5555555555`,
  `aaaaaaaaaa`) get about 2%; Nordic advises against such addresses anyway. The
  sensitivity cost of the stricter threshold is unknown; it is only used for
  weak addresses.

### 3. No packet engine for the NRF and XN297 formats (software load)

- What: the RF core is used as preamble + sync word + fixed length raw data.
  Address check, NRF and XN297 CRCs, scrambling, the bit-shifted 9 and 10 bit
  packet control fields, retransmissions and ack matching are done in
  software.
- Impact: it works and is tested (288 bit-exact cases against the MPM XN297
  code, NRF packets captured over the air), but every sync is checked by the
  CPU, packets with a dynamic length must be received at their maximum length
  (the radio stays busy longer than the packet), and each new protocol variant
  needs software framing.

### 4. Opaque RF core (engineering risk)

- What: the RF core runs TI binary patches. Key tuning (sync threshold
  `0x5114`, AGC reference `0x609C`) uses registers only documented by values in
  TI's SmartRF settings. The behaviors above were found by measurement, not
  from documentation.
- Impact: each new PHY (CC2500, A7105 or CYRF6936 emulation) may need trial
  and error tuning with little vendor documentation to lean on.

### 5. Not evaluated yet

- Protocols: none left, every MPM protocol of the four chips has been run
  (CFlie at 2 Mbps excepted, weakness 1).
- RF: sensitivity, range and blocking are unmeasured. The CC2652 receives the
  CYRF6936 chips as plain 1 Mbps GFSK bits and correlates the codes in
  software: the CYRF6936 DSSS processing gain is only partly kept (up to 20
  chip errors per 64 chip SDR bit are corrected, but the sync word itself is
  32 raw chips). Frequency tolerance at 1
  Mbps: 100% at 250 kHz offset, 0% at 350 kHz (about 100 ppm at 2.44 GHz: fine
  for normal crystals, but a limit).

### 6. Frequency trim needed for the narrow CC2500 PHYs

- What: the CC2500 receives the CC2652 FrSky D8 packets only from about -60 to
  0 kHz of CC2652 detuning on these two boards (15 ppm apart). At 0 kHz:
  90 to 100% depending on the run, FrSky X LBT 92.5%.
- Impact: none once trimmed (`trim -35`: 100%). The MPM has the same need
  (`option` = CC2500 `FSCTRL0`), so the CC2652 firmware needs a per-module
  frequency correction, settable the same way. The CC2652 RX (175 kHz filter
  for D8) takes the CC2500 without correction.
- The A7105 needs it too: this one is about 90 kHz above the CC2652 (37 ppm).
  Without trim the A7105 loses 25 to 45% at 500 kbps (FlySky, AFHDS2A).

## CC2500 protocol family

Every MPM CC2500 protocol decoded from its registers (26 MHz crystal):

| Protocols | PHY | Framing | CC2652 status |
|---|---|---|---|
| FrSky D8 | 31 kbps GFSK, 32 kHz dev | 16 bit sync D391, length, CRC | works |
| FrSky V8 | 27 kbps GFSK, 29 kHz dev | 32 bit sync, length, CRC | works |
| FrSky X / X2, FrSky RX | 70 / 77 kbps GFSK, 57 kHz dev | 32 bit sync, length, CRC (X2) | works |
| FrSky X / X2 LBT | 100 kbps 2-FSK, 70 kHz dev | 32 bit sync, length, CRC (X2) | works |
| Corona V1/V2, Flydream V3 | 100 kbps 2-FSK, 48 to 51 kHz dev | 32 bit sync, length, CRC | works, bind up to 2483.6 MHz |
| Ares | 50 kbps 2-FSK, 38 kHz dev | 32 bit sync 5A60, length, CRC | works |
| Futaba SFHSS | 128 kbps 2-FSK, 38 kHz dev | 32 bit sync, fixed 13, CC2400 CRC | works (CRC measured, see below) |
| Redpine slow | 10 kbps 2-FSK, 38 kHz dev | 32 bit sync, length, CRC | works |
| RadioLink (all) | 38 kbps GFSK, 38 kHz dev | 32 bit sync ABCD / BADC / ABA5, length, CRC, whitening | works |
| HoTT | 250 kbps MSK | 32 bit sync, fixed length, CRC, whitening, FEC | works |
| Hitec | 250 kbps MSK (2/8 phase step), 24 byte preamble | 32 bit sync from the ID, length, CRC, whitening | works |
| Redpine fast | 250 kbps MSK (1/8) | 32 bit sync, length, CRC | works, up to 2502.2 MHz (above TI's 2500 MHz spec, this sample) |
| Skyartec | 229 kbps MSK (1/8) | 32 bit sync from the ID, length, CRC | works |
| IKEA Ansluta | 250 kbps MSK (2/8) | length, CRC, FEC | works |
| FrSky L (LR12) | 125 kchip/s 2-FSK, no sync | raw 256 byte chip stream, 11 chips per bit | works |
| XN297L / NRF250K emulations (Bumblebee, MT99xx, OMP, V911S, ESky150 v2...) | 250 kbps GFSK, 127 kHz dev | software (XN297 / NRF framing) | works (XN297 250 kbps tested) |

Measured with HoTT:

- CC2500 MSK with the full 8/8 phase step is 2-FSK with a 0.5 modulation
  index on air (62.5 kHz at 250 kbps), with all bits inverted (sync word and
  preamble too). The CC2652 sends and receives it as 2-FSK.
- CC2500 FEC (TI DN504: rate 1/2 convolutional code, 4x4 interleaving,
  trellis terminator) and PN9 whitening are done in software, bit exact
  against the CC2500. RX uses a hard decision Viterbi decoder (8 states):
  isolated bit errors are corrected, as on the CC2500. The packet doubles on
  air: 108 bytes for HoTT's 50, received as fixed length raw data.

MSK with a partial phase step (`DEVIATN` M below 7: Hitec, IKEA 2/8, Redpine
fast and Skyartec 1/8) works as 2-FSK too, with one deviation for both
directions: the CC2500 wants more than true MSK (Hitec: 50 kHz 71%, 62.5 kHz
99.5%, 75 to 160 kHz 100%), the CC2652 receiver less than about 0.4 x rate
(Skyartec: 100 kHz 0%) with a 350 kHz filter. 0.32 x rate gives 100% both ways
for all four.

Other findings:

- The `CC2400_EN` CRC (SFHSS) is not documented: measured as CRC-16 0x8005
  from 0 over all bytes but the last 2, XORed into it unprocessed.
- FEC with a variable length (IKEA) works on the CC2500, unlike what its
  datasheet says, and on the CC2652 (length decoded first).
- Redpine fast hops up to 2502.2 MHz: this CC2652R sample works there (100%),
  outside TI's 2360 to 2500 MHz specification. Not guaranteed.
- FrSky L sends a 256 byte chip stream with no sync word: 6 header bytes 0xAA
  and the packet, each bit (LSB first) as 11 chips (`0x0ED` / `0x712`), then
  `55 55 55`, at 125 kbaud 2-FSK (47.6 kHz deviation). The CC2652 `frskyl`
  framing sends it bit exact (`FrSkyL_encode_packet()`); for RX it syncs on
  the header end and the constant first byte (0x4E) and decodes each bit to
  the nearest code. The CC2500 sends no preamble before it (none found); the
  CC2652 sends 4 bytes.
- While receiving FrSky L on one channel, the CC2652 also got the packets of
  the channel 48 MHz higher or lower (its crystal frequency), about 80 dB
  weaker. A spurious response to keep in mind for blocking tests.
- Corona V2 freezes the CC2500 AGC: at bench signal levels (-18 dBm) the
  CC2500 then receives nothing, from any transmitter. Not a CC2652 issue.

## A7105 protocol family

| Protocols | PHY (`DATA_RATE`, `CODE_I`) | Framing | CC2652 status |
|---|---|---|---|
| FlySky (AFHDS) | 500 kbps GFSK | ID sync, fixed 21, CRC | works |
| AFHDS2A (+RX) | 500 kbps GFSK | ID sync, fixed 38, CRC, FEC | works |
| Hubsan | 100 kbps GFSK | ID 55201041, fixed 16, no CRC | works |
| Kyosho FHSS / Syncro | 500 kbps | ID sync, fixed 38, CRC, FEC | works |
| WFLY2, Joysway | 500 kbps | ID sync, fixed 32 / 16, CRC | works (A7105 RX: 98%, CRC errors) |
| Bugs | 250 kbps | ID sync, fixed 22, CRC | works |
| Height (5 / 8 channels) | 250 kbps | ID 25A53C45, fixed 8 / 11, CRC, FEC | works (A7105 RX of 8 bytes: 88 to 98%, see below) |
| Pelikan | 250 kbps (125 kbps also tried) | ID 06230623, fixed 16, no CRC | works |
| Kyosho Hype | 250 kbps | ID sync, fixed 17, CRC, FEC | works |

Measured on raw captures of the A7105 (bit exact): preamble, the 4 byte ID
(as given to `A7105_WriteID()`) as the sync word, the fixed length payload,
MSB first, no whitening. CRC-16 CCITT (0x1021) from 0x1D0F over the payload.
FEC: each nibble (high first) as a Hamming (7,4) code word, data bits first,
14 bits per byte, padded to a byte at the end; the CC2652 decodes to the
nearest code word (1 bit error corrected per word, as the A7105). 500 kbps /
(`DATA_RATE` + 1), channels 2400 MHz + `PLL_I` x 500 kHz.

The A7105 receives the CC2652 best with a 250 kHz deviation (`TX_II` 0x2B):
FlySky without FEC 2.4% CRC errors at 186 kHz, 0.6% at 250 kHz, worse at 140
and 300 kHz; the CC2652 receives the A7105 at all of them. The 0.2 to 1% left at
500 kbps doesn't depend on the CC2652 power; without a second A7105 there is
no reference to compare it to. At 2410 MHz (Wi-Fi channel 1) the A7105
also lost about one packet per 100 ms, likely Wi-Fi beacons; the CC2652
receiver did not.

The other A7105 protocols were run with their own `A7105_Init()` register
tables (bench presets), 500 packets each way. `CODE_I` bit 3 enables the CRC,
bit 4 the FEC. Notes:

- Most of them only transmit, and their tables don't receive on the bench. The
  CC2652 -> A7105 runs use the AFHDS2A table set to each protocol's rate,
  `CODE_I`, FIFO length and ID. The packet format is the same, but the A7105
  receiver isn't tested with the protocols' own tables.
- Bugs (which receives telemetry) receives at `PLL_I` - 2 with its own table,
  as `Bugs_a7105.ino` sets it; it doesn't receive on the bench with it.
  Bugs' format with the AFHDS2A table: 99%.
- Height 5 channels (8 byte packets): the A7105 missed 2 to 12% of the
  packets, without CRC errors, while Height 8 channels (11 bytes) got 100%.
  This looks like the bench's A7105 RX restart between short packets, not
  the format.

## CYRF6936 protocol family

| Protocols | `TX_CFG` mode | Framing | CC2652 status |
|---|---|---|---|
| DSM2 / DSMX data and telemetry | 64 chip 8DR | SOP, length, CRC-16 with seed | works |
| DSM bind, bind response | 64 chip SDR | no SOP, length or CRC, 16 / 10 bytes | works |
| J6Pro, WK2x01 | 64 chip 8DR | SOP, length, CRC | works |
| Scorpio | 64 chip 8DR | default SOP, length, CRC seed 1 | works |
| Kyosho3 | 64 chip 8DR | 32 chip SOP (`FRAMING_CFG` 0xA4), length, no CRC, all chips inverted (`TX_OVERRIDE` TX_INV) | works |
| MLINK | 64 chip SDR | no SOP, length or CRC | works |
| Losi | 64 chip SDR | as DSM bind | works |
| Devo, Traxxas | 32 chip 8DR | 32 chip SOP, length, CRC (Traxxas: seed 0x5a5a) | works |
| WFLY | 32 chip 8DR | 64 chip SOP, length, CRC | works |
| E01X, E129, E010R5 | GFSK 1 Mbps (no DSSS) | HS6200 / RF2500, built in software | works, see below |

Measured on raw captures of the CYRF6936 (bit exact), at 1 Mchip/s, in CC2652
bits:

- Preamble `1100`, 16 chips per count (DSM: 4, the default and Devo: 2).
- SOP code (as given to `CYRF_ConfigSOPCode()`, LSB first; 32 chip SOP: its
  first 4 bytes), 4 alternating chips starting with the last one inverted, the
  SOP code again, inverted only with 64 chip data codes.
- A gap chip (the last chip inverted), then the data. 8DR, 64 chip codes: one
  symbol per byte, the data code (bit 6: the second one) rotated by bits 0-5,
  inverted unless bit 7. 32 chip codes: 4 data codes (bits 5-6) rotated by bits
  0-4. SDR: one 64 chip code per bit, LSB first, inverted for a 0.
- The length byte first, then the payload and a CRC-16 (0xA001 reflected, from
  the bit-reversed seed, xorout 0xFFFF), low byte first. No end of packet code.
- Devo doesn't set the data codes: the CYRF6936 default is
  `6fbe3201dbf12b01e35cfa029793f902`, and the default SOP code
  `82c79036219eff17` (Scorpio, Kyosho3). J6Pro's `j6pro_data_code` is that
  default in reversed byte order, despite its comment.
- `TX_OVERRIDE` TX_INV (Kyosho3) inverts every chip, the preamble included.

Frequencies (`CYRF_ConfigRFChannel()` n, measured): TX at 2400 + n MHz, or
2401 + n with `TX_OFFSET` 0x0555 (set by most protocols, not Kyosho3, MLINK or
the GFSK mode). RX at about 2401.2 + n with `RX_CFG` FAST_TURN_EN (bit 3) and
bit 4 clear (0x4A), at 2402 + n otherwise (0x52, 0x42, 0x5A, 0x12). On the
image frequency the CYRF6936 receives inverted data. So a protocol's own TX and
RX frequencies can differ by 1 MHz; the CC2652 follows each one with its
channel plan. 250 kHz deviation (the CYRF6936 still receives with the CC2652
250 kHz off, not at 500 kHz).

The protocols were run with their own register setups (bind configuration,
one channel), 100 packets each way. Most of them only transmit; for
CC2652 -> CYRF6936 the bench adds what their setup lacks to receive:
`CLK_EN` 0x02 (Scorpio, Kyosho3), a first RX that gets nothing (WFLY), RX CRC
checking off (Losi). MLINK sets `RX_CFG` VLD_EN: each received byte is then
followed by a valid flag byte; the bench turns it off. With MLINK's receiver
at 2402 + n the bench CC2652 had to send inverted chips; this isn't
understood, and MLINK's own packets are not inverted.

The CC2652 side:

- 8DR packets: sync in the SOP part (a sync on the preamble missed most). Each
  symbol is decoded against all the codes, rotations and polarities, the
  nearest wins: 8 chip errors per 64 chip symbol and 4 per 32 chip symbol are
  corrected in the unit tests.
- With 32 chip data codes the two SOP copies are the same, 36 chips apart: a
  sync word on the second also matched the first and no packet decoded. Sync
  windows are scored against all the earlier known bits, not only the 32
  before them (the same weakness as weakness 2): the sync word then spans the
  separator. The Devo 32 chip preamble is enough for the CC2652.
- SDR packets without SOP (DSM bind): only the repetitive preamble is known
  before the data, and a sync word on it got 25 to 35% at any threshold. The
  sync word is the preamble end, the gap and the first 23 chips of the first
  bit, with `CMD_PROP_RX_ADV`'s alternative sync word for the other bit value
  and the strict threshold: 100%. When the first bytes are known (bind
  response: the inverted manufacturer id of the TX), the sync word is in the
  data: 100%.
- An SDR bit takes 8 bytes on air: the 16 byte bind packet is 1029 bytes, the
  most a single CC2652 packet needs (`RADIO_MAX_PKT_LEN` 1100). Basic
  `CMD_PROP_TX` / `RX` stop at 255 bytes.

## CYRF6936 GFSK mode (E01X, E129, E010R5)

These protocols put the CYRF6936 in its 1 Mbps GFSK mode (`CYRF_GFSK1M_Init()`:
no SOP, length or CRC) and build the frames of other chips in software:
HS6200 (`HS6200_EMU.ino`) and RF2500 (`RF2500_EMU.ino`). On the CC2652 they are
plain 1 Mbps GFSK packets, framed by `frame.c` (`FRAME_HS6200`,
`FRAME_RF2500`). Measured on the CYRF6936:

- Bytes are sent LSB first (the MPM bit-reverses its buffer for that), so the
  frames are MSB first on air.
- The preamble is 0xAA on air, whatever the first address bit: about 2 bytes
  for E01X (preamble count 1). The CC2652 sends the 0xAA byte as part of the
  frame, so its preamble matches.
- Channel n at 2400 + n MHz (`TX_OFFSET` 0).

| Framing | Protocols | Format |
|---|---|---|
| HS6200 | E01X (E012, E015) | address (last byte first), 2 guard bytes (its last byte again), 9 bit packet control field (6 bit length, 2 bit pid, no ack bit set), payload XORed with a 15 byte table, CRC-16 CCITT from 0xFFFF over the address, control field and payload |
| RF2500 | E129, C186 (16 / 19 bytes, scrambled), E010R5 (14 bytes) | 4 byte address, `C3 C3`, then payload and CRC with each bit (LSB first) sent as 4: `1100` / `0011`. CRC-16 CCITT from 0 over the bits as sent; scrambled: payload XORed with a table, CRC with a value per length |

The CC2652 encoder matches the MPM code bit for bit (unit test, random
payloads). The CC2652 received the MPM's own E012, E015, E129, C186 and
E010R5 bind packets and E015 data packets, all of them with the right
payload. Two MPM bugs showed up:

- HS6200 CRC: `HS6200_calc_crc()` takes the last CRC bit from the byte after
  the payload (`msg[pos+1]` instead of `msg[pos]`). That byte isn't written
  yet: it holds stack contents. `crc16_update()` also mixes all 8 bits of it
  into the CRC, not one. Each MPM CRC was explained by exactly one value of
  that byte, and the value changed from packet to packet. The CRC was right
  for 3 to 4% of the E012 bind packets, 95 to 100% of the E015 bind packets
  (by chance) and 0% of the E015 data packets. Either the models don't check it or E01X
  doesn't work. The CC2652 sends the CRC as intended (`msg[pos]`). Without an
  E01X model nobody can tell which one it accepts. The MPM fix,
  `crc16_update(msg[pos], 1)`, is documented in
  [interop-results.md](interop-results.md); it is not applied until someone
  tests it with a model.
- E010R5: the callback changes the channel right after
  `RF2500_SendPayload()`, while the CYRF6936 still sends the last bytes
  (`CYRF_GFSK1M_SendPayload()` returns when its FIFO is filled). The last
  packet before each hop, 1 in 6, arrived corrupted. Fixed in the MPM (the
  hop waits for the end of the packet,
  [PR #1197](https://github.com/pascallanger/DIY-Multiprotocol-TX-Module/pull/1197)): 100%. The CC2652 `radio_send()` waits
  for the end of the packet anyway.

## What any candidate chip must show

Requirements that came out of these tests, to check on each alternative:

1. GFSK RX and TX at 250 kbps, 1 Mbps and 2 Mbps with a 1 byte preamble
   (nRF24L01, XN297).
2. A free 32 bit sync word that copes with repeated addresses such as
   `e7e7e7e7e7`.
3. TX to RX turnaround well under 130 us, for Enhanced ShockBurst acks.
4. A raw mode without forced whitening or CRC, to emulate the CC2500, A7105
   and XN297 formats.
5. A way to emulate the CC2500, A7105 and CYRF6936 PHYs, DSM's DSSS included:
   2-FSK and GFSK from 10 kbps to 1 Mbps, MSK, a 16 or 32 bit sync word after
   a 2 byte preamble, frequencies up to 2502 MHz (Redpine). On the CC2652: the
   CC2500, A7105 and CYRF6936 families done.
6. Raw packets of at least 1029 bytes at 1 Mbps (DSM bind: 128 SDR bits of 64
   chips), or a streaming RX/TX mode, and an alternative RX sync word (or a
   sync word placed anywhere in the known bits) for the DSM bind packets.
7. Documentation or vendor support good enough to tune the demodulator without
   reverse engineering.
8. A per-module frequency trim, a few kHz steps over at least +-100 kHz.

## Next candidate: CC2745R10

Since every protocol but one works on the CC2652, a newer chip of the same
vendor is the natural next step: the CC2745R10-Q1 (LaunchPad
LP-EM-CC2745R10-Q1). From its datasheet (SWRS304D) and product page, not
measured:

- Arm Cortex-M33 at 96 MHz, 1 MB flash, 162 KB SRAM. Up to +10 dBm (R
  variant); the P variant has a +20 dBm PA, which an MPM needs anyway.
  2360 to 2500 MHz.
- Bluetooth LE 1M, 2M and Coded PHYs, with -93 dBm sensitivity at 2 Mbps (LE
  2M): the demodulator does run at 2 Mbps. That is the CC2652's one blocking
  gap (weakness 1), if 2 Mbps is usable outside the BLE packet format.
- A new radio core (LRF): a "software-defined digital radio" with a
  command-based API, whose modem "is not programmable by customers but is
  instead loaded with precompiled images" from the SimpleLink Low Power F3
  SDK. The datasheet only describes BLE. The LaunchPad page also lists 2.4 GHz
  proprietary RF, through the SDK's radio driver (RCL).

What carries over: `frame.c`, `bench.c` and the whole rig are chip-agnostic.
The framings are software over a raw "preamble + sync word + fixed length"
radio. Only `radio.c` (the RF core commands, the sync threshold register, the
CPE patch) would have to be rewritten for the LRF. The rig's suites would then
give the same interoperability tables for the new chip.

What to check first, in this order:

1. A raw proprietary mode at 2 Mbps in RX: GFSK with a free 32 bit sync word
   and no BLE access address, whitening or length field forced. This decides
   CFlie.
2. Free symbol rate and deviation from 10 kbps to 1 Mbps, 2-FSK and GFSK
   (the CC2500, A7105 and CYRF6936 PHYs), and frequencies up to 2502 MHz.
3. Packets above 1 KB or a streaming mode (DSM bind), and an alternative RX
   sync word.
4. Control over the sync correlation threshold: the CC2652 needed it for
   repeated addresses and DSM bind. Closed modem images may hide it.
5. TX to RX turnaround for ESB acks, and the channel change time.
