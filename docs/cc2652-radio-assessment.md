# CC2652 radio assessment

What the CC2652R RF core can and cannot do for the MULTI-Module protocols,
measured over the air against a 4-in-1 module, as input for the chip choice.

Measured 2026-10-18: CC2652R1 LaunchPad (PCB antenna) next to an iRangeX IRX4
Lite running the MPM bench firmware, 200 to 250 packets per test, signal around
-24 dBm. Test commands and per-test results: `Bench/README.md` in
DIY-Multiprotocol-TX-Module (branch `bench-mode`). CC2652 code: `src/radio.c`
(RF core driver) and `src/frame.c` (NRF24L01 / XN297 / CC2500 / A7105 framing), built
with `-DBENCH=ON`.

## Summary

- Works: NRF24L01 and XN297 protocols at 250 kbps and 1 Mbps, both
  directions, including Enhanced ShockBurst auto-ack with retransmissions and
  ack payloads. Bare-metal, with two TI files (`rfc.c`/`rfc.h` and the
  proprietary CPE patch).
- Hard limit: no 2 Mbps reception. CFlie models set to 2 Mbps would not work.
- Works: every MPM CC2500 protocol tried, both directions: FrSky D8/V8/D16
  (FCC and LBT), HoTT, Hitec, Redpine, Skyartec, Corona, Flydream, Ares,
  RadioLink, IKEA, SFHSS. That covers 2-FSK, GFSK and MSK from 10 to 250 kbps,
  whitening, FEC and the CC2400 CRC, with a per-board frequency trim like the
  MPM `option`. Only FrSky L (TX only, raw chip stream) is untested. FrSky
  itself runs ACCST and ACCESS on CC26xx chips in its receivers and transmitter
  modules (information from the project owner, not a bench result).
- Works: the A7105 protocols tried, FlySky, AFHDS2A (FEC) and Hubsan: 100%
  from the A7105, 99 to 100% to it (A7105 CRC errors at 500 kbps), with a
  90 kHz trim on this board.
- Mitigated in software: repeated address patterns. Degenerate addresses such
  as `5555555555` still fail.
- Biggest unknowns: the CYRF6936 family (DSM, Walkera Devo), and receiver
  sensitivity.

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
| CC2500 FrSky L (raw chip stream) | not tested | |
| A7105 FlySky, AFHDS2A (500 kbps, CRC, FEC), Hubsan (100 kbps) | works | 100% A7105 -> CC2652, 99 to 99.8% (500 kbps) and 100% (Hubsan) CC2652 -> A7105, with a 90 kHz trim |
| Other A7105 protocols (Bugs, Height, Pelikan, Kyosho, WFLY2, Joysway) | not tested | same PHY family, 250 or 500 kbps |
| CYRF6936 protocols | not tested | |
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

- Protocols: the CYRF6936 (DSM) family hasn't been run on the CC2652. DSM
  uses DSSS codes, the most likely to be a problem for a GFSK packet radio.
- RF: sensitivity, range and blocking are unmeasured. Frequency tolerance at 1
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
| FrSky L (LR12) | 125 kchip/s 2-FSK, no sync | raw 256 byte chip stream, 11 chips per bit | not tested: TX only, fits a 32 bit sync + 252 byte packet |
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
- Corona V2 freezes the CC2500 AGC: at bench signal levels (-18 dBm) the
  CC2500 then receives nothing, from any transmitter. Not a CC2652 issue.

## A7105 protocol family

| Protocols | PHY (`DATA_RATE`, `CODE_I`) | Framing | CC2652 status |
|---|---|---|---|
| FlySky (AFHDS) | 500 kbps GFSK | ID sync, fixed 21, CRC | works |
| AFHDS2A (+RX) | 500 kbps GFSK | ID sync, fixed 38, CRC, FEC | works |
| Hubsan | 100 kbps GFSK | ID 55201041, fixed 16, no CRC | works |
| Kyosho FHSS / Syncro, WFLY2, Joysway | 500 kbps | ID sync, CRC (Kyosho: FEC) | not tested, same PHY as FlySky |
| Bugs, Height, Pelikan, Kyosho Hype | 250 kbps | ID sync, CRC (Height, Hype: FEC, Pelikan: no CRC) | not tested |

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
   2-FSK and GFSK from 10 to 500 kbps, MSK, a 16 or 32 bit sync word after a
   2 byte preamble, frequencies up to 2502 MHz (Redpine). On the CC2652: the
   CC2500 and A7105 families done, the CYRF6936 PHYs open.
6. Documentation or vendor support good enough to tune the demodulator without
   reverse engineering.
7. A per-module frequency trim, a few kHz steps over at least +-100 kHz.
