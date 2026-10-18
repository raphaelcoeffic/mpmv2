# CC2652 radio assessment

What the CC2652R RF core can and cannot do for the MULTI-Module protocols,
measured over the air against a 4-in-1 module, as input for the chip choice.

Measured 2026-10-18: CC2652R1 LaunchPad (PCB antenna) next to an iRangeX IRX4
Lite running the MPM bench firmware, 200 to 250 packets per test, signal around
-24 dBm. Test commands and per-test results: `Bench/README.md` in
DIY-Multiprotocol-TX-Module (branch `bench-mode`). CC2652 code: `src/radio.c`
(RF core driver) and `src/frame.c` (NRF24L01 / XN297 / CC2500 framing), built
with `-DBENCH=ON`.

## Summary

- Works: NRF24L01 and XN297 protocols at 250 kbps and 1 Mbps, both
  directions, including Enhanced ShockBurst auto-ack with retransmissions and
  ack payloads. Bare-metal, with two TI files (`rfc.c`/`rfc.h` and the
  proprietary CPE patch).
- Hard limit: no 2 Mbps reception. CFlie models set to 2 Mbps would not work.
- Works: the FrSky CC2500 PHYs (D8, D16 v1 and v2, FCC and LBT) and HoTT
  (MSK, whitening, FEC), both directions, with a per-board frequency trim like
  the MPM `option`. FrSky
  itself runs ACCST and ACCESS on CC26xx chips in its receivers and transmitter
  modules (information from the project owner, not a bench result).
- Mitigated in software: repeated address patterns. Degenerate addresses such
  as `5555555555` still fail.
- Biggest unknowns: the A7105 and CYRF6936 families (FlySky, Hubsan, DSM),
  receiver sensitivity, and MSK with a partial phase step (Hitec, Redpine fast,
  Skyartec, IKEA).

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
| Other CC2500 protocols | not tested | see "CC2500 protocol family" |
| A7105, CYRF6936 protocols | not tested | |
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

- Protocols: the rest of the CC2500 family (below), the A7105 (FlySky
  AFHDS/AFHDS2A, Hubsan) and CYRF6936 (DSM) families haven't been run on the
  CC2652. DSM uses DSSS codes, the most likely to be a problem for a GFSK
  packet radio. The MPM bench already has A7105 presets for these tests.
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

## CC2500 protocol family

Every MPM CC2500 protocol decoded from its registers (26 MHz crystal):

| Protocols | PHY | Framing | CC2652 status |
|---|---|---|---|
| FrSky D8 | 31 kbps GFSK, 32 kHz dev | 16 bit sync D391, length, CRC | works |
| FrSky X / X2, FrSky RX | 70 / 77 kbps GFSK, 57 kHz dev | 32 bit sync, length, CRC (X2) | works |
| FrSky X / X2 LBT | 100 kbps 2-FSK, 70 kHz dev | 32 bit sync, length, CRC (X2) | works |
| FrSky V8 | 27 kbps GFSK, 29 kHz dev | 32 bit sync, length, CRC | expected to work (same as D8) |
| Corona V1/V2, Flydream V3 | 100 kbps 2-FSK, 48 to 51 kHz dev | 32 bit sync, length, CRC | expected to work. V1/FDV3 bind at 2483.6 MHz |
| Ares | 50 kbps 2-FSK, 38 kHz dev | 32 bit sync 5A60, length, CRC | expected to work |
| Futaba SFHSS | 128 kbps 2-FSK, 38 kHz dev | 32 bit sync, fixed 13, CC2400 style CRC | CRC to identify (`CC2400_EN`) |
| Redpine slow | 10 kbps 2-FSK, 38 kHz dev | 32 bit sync, length, CRC | expected to work |
| RadioLink (all) | 38 kbps GFSK, 38 kHz dev | 32 bit sync ABCD / BADC / ABA5, length, CRC, whitening | expected to work (whitening done for HoTT) |
| HoTT | 250 kbps MSK | 32 bit sync, fixed length, CRC, whitening, FEC | works |
| Hitec | 250 kbps MSK (2/8 phase step), 24 byte preamble | 32 bit sync from the ID, length, CRC, whitening | MSK phase step to check |
| Redpine fast | 250 kbps MSK | 32 bit sync, length, CRC | MSK, and channels up to 2502 MHz, above the CC2652 range (2500 MHz) |
| Skyartec | 229 kbps MSK | 32 bit sync from the ID, length, CRC | MSK |
| IKEA Ansluta | 250 kbps MSK | length, CRC, FEC | MSK, FEC |
| FrSky L (LR12) | 125 kchip/s 2-FSK, no sync | raw 256 byte chip stream, 11 chips per bit | raw continuous TX of a precomputed stream |
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

What remains risky:

- MSK with a partial phase step (`DEVIATN` M below 7: Hitec, IKEA 2/8,
  Redpine fast and Skyartec 1/8): the frequency peaks are higher than with
  2-FSK, the net phase per symbol the same. Needs measuring.
- Redpine fast hops up to 2502 MHz, above the CC2652R synthesizer range.

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
   2-FSK and GFSK from 10 to 250 kbps, MSK, a 16 or 32 bit sync word after a
   2 byte preamble, frequencies up to 2502 MHz (Redpine). On the CC2652: FrSky
   done, MSK and the A7105 / CYRF6936 PHYs open.
6. Documentation or vendor support good enough to tune the demodulator without
   reverse engineering.
7. A per-module frequency trim, a few kHz steps over at least +-100 kHz.
