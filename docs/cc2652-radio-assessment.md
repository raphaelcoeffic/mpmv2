# CC2652 radio assessment

What the CC2652R RF core can and cannot do for the MULTI-Module protocols,
measured over the air against a 4-in-1 module, as input for the chip choice.

Measured 2026-10-18: CC2652R1 LaunchPad (PCB antenna) next to an iRangeX IRX4
Lite running the MPM bench firmware, 200 to 250 packets per test, signal around
-24 dBm. Test commands and per-test results: `Bench/README.md` in
DIY-Multiprotocol-TX-Module (branch `bench-mode`). CC2652 code: `src/radio.c`
(RF core driver) and `src/frame.c` (NRF24L01 / XN297 framing), built with
`-DBENCH=ON`.

## Summary

- Works: NRF24L01 and XN297 protocols at 250 kbps and 1 Mbps, both
  directions, including Enhanced ShockBurst auto-ack with retransmissions and
  ack payloads. Bare-metal, with two TI files (`rfc.c`/`rfc.h` and the
  proprietary CPE patch).
- Hard limit: no 2 Mbps reception. CFlie models set to 2 Mbps would not work.
- Mitigated in software: repeated address patterns. Degenerate addresses such
  as `5555555555` still fail.
- Biggest unknown: the CC2500, A7105 and CYRF6936 protocol families (FrSky,
  FlySky, Hubsan, DSM) have not been tried on the CC2652 yet, nor has receiver
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
| CC2500, A7105, CYRF6936 protocols | not tested | |
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

- Protocols: the CC2500 (FrSky D8/D16...), A7105 (FlySky AFHDS/AFHDS2A,
  Hubsan) and CYRF6936 (DSM) families are the most used MPM protocols and
  haven't been run on the CC2652. DSM uses DSSS codes, the most likely to be a
  problem for a GFSK packet radio. The MPM bench already has CC2500 and A7105
  presets for these tests.
- RF: sensitivity, range and blocking are unmeasured. Frequency tolerance at 1
  Mbps: 100% at 250 kHz offset, 0% at 350 kHz (about 100 ppm at 2.44 GHz: fine
  for normal crystals, but a limit).

## What any candidate chip must show

Requirements that came out of these tests, to check on each alternative:

1. GFSK RX and TX at 250 kbps, 1 Mbps and 2 Mbps with a 1 byte preamble
   (nRF24L01, XN297).
2. A free 32 bit sync word that copes with repeated addresses such as
   `e7e7e7e7e7`.
3. TX to RX turnaround well under 130 us, for Enhanced ShockBurst acks.
4. A raw mode without forced whitening or CRC, to emulate the CC2500, A7105
   and XN297 formats.
5. A way to emulate the CC2500, A7105 and CYRF6936 PHYs, DSM's DSSS included.
   This is the open question for the CC2652 too.
6. Documentation or vendor support good enough to tune the demodulator without
   reverse engineering.
