# MPM v2 radio: status and next steps

The goal: a MULTI-Module built around one 2.4 GHz SoC instead of the four
chips of the 4-in-1 (CC2500, NRF24L01, A7105, CYRF6936), running the MPM
protocols with the same functionality: TX, telemetry and bind RX, and the RX
protocols. The CC2652 is the first candidate assessed: first on a CC2652R1
LaunchPad, now on an MPM v2 prototype board (CC2652RB, SKY66112 PA/LNA, see
[hardware.md](hardware.md)). Neither the chip nor the board is settled.

Status in October 2026: on the bench, the CC2652 sends and receives the
on-air format of every MPM protocol of the four chips, against the chips of a
real 4-in-1 module, from the LaunchPad and from the MPM v2 board. The one
exception is CFlie at 2 Mbps. No protocol runs on the CC2652 yet: what exists
is the radio driver, the framing layer, the MPM v2 board support and the test
rig.

## Done

**CC2652 radio driver** (`src/radio.c`): the RF core in proprietary mode,
bare-metal. Only `rfc.c`/`rfc.h` and the proprietary CPE patch come from TI's
radio code. It is a raw packet engine: preamble, a 32 bit sync word (plus an
alternative one), fixed-length data up to 1100 bytes (the advanced commands
above 255). It also has chained TX -> RX for Enhanced ShockBurst acks, a
settable sync correlation threshold and a frequency trim. On the CC2652RB it
corrects the BAW oscillator (HPOSC) from TI's factory calibration and the chip
temperature: about 98 ppm fast on the MPM v2 board, within about 10 kHz of the
LaunchPad once corrected, no per-board calibration.

**MPM v2 board support** (`src/board.c`, `src/board.h`): the pinout from the
schematic and the RF front end set up at boot: RF switch to the CC2652, the
PA/LNA switched by the RF core (`RFC_GPO1`), antenna 1, SX1281 held in reset,
module inverters idle. TX power default `0x2853` (0 dBm into the PA, about
+20 dBm out, within the SKY66112 input limit). Pins, RF chain and the changes
for the next revision: [hardware.md](hardware.md).

**Framing layer** (`src/frame.c`): the packet formats of the MPM chips, built
and decoded in software over the raw radio:

- NRF24L01, with Enhanced ShockBurst (auto-ack, retransmissions, ack
  payloads).
- XN297, standard and enhanced modes.
- CC2500: CRC, the CC2400 CRC, PN9 whitening, FEC with a Viterbi decoder,
  MSK.
- A7105: CRC, Hamming FEC.
- CYRF6936 DSSS: 8DR and SDR, 32 and 64 chip codes, SOP.
- The HS6200 and RF2500 formats that the CYRF6936 GFSK mode emulates.
- The FrSky L chip stream.

The RX sync word is chosen among all the bits known in advance, so that it
doesn't trigger early on repeated patterns. Much of this format knowledge was
measured on air, because the datasheets don't give it:

- the CC2400 CRC and the FEC behaviour of the CC2500;
- the CYRF6936 SOP, separator and gap chips, its 32 chip codes and its real TX
  and RX frequencies;
- the A7105 CRC and FEC;
- the ESB packet control field, as the nRF24L01+ sends it.

**Test rig**: the two bench firmwares speak the same line protocol, and one
set of host tools drives both (`tools/bench`, see [bench.md](bench.md)).

- On the MPM side, the bench firmware is branch `mpm-bench` of the MPM fork.
- The CC2652 side runs on the LaunchPad or the MPM v2 board (`-p v2`,
  `CC_PORT=v2`): USB-C console, flashing with a J-Link on J10 through OpenOCD
  (`ninja flash`).
- An ESP32-S3 with esp-sdr is a third receiver: it shows what each chip
  really sends (`sdr_compare.py`).
- The rig has suites per chip family, captures of the real MPM protocols, and
  frequency scans and raw captures for reverse engineering.
- 12 host unit tests (`tests/frame`) check `frame.c` against the MPM encoders
  and against raw captures.

**Results** ([interop-results.md](interop-results.md)):

- NRF24L01 and XN297 work at 250 kbps and 1 Mbps both ways, ESB acks and ack
  payloads included.
- All 18 CC2500 setups, 11 A7105 and 12 CYRF6936 DSSS setups run with their
  own register tables, mostly 97 to 100% each way.
- E01X, E129, C186, E010R5 and FrSky L run with the real MPM protocol code.
- Not possible: 2 Mbps RX (CFlie).
- The MPM v2 board gives the same results as the LaunchPad: 96 to 100% from
  the MPM chips, 95 to 100% to them. The losses below 100% that were tracked
  down are on the bench side: output stalls and a race in the MPM bench
  firmware (fixed: CC2500 and CYRF6936 now 100% to the MPM but Corona V2 and
  Kyosho3), the Corona V2 frozen AGC, and Wi-Fi beacons and traffic against
  the CYRF6936 bind power (PA 0, about -50 dBm).
  Still unexplained: [below](#losses-still-to-explain).

What it means for the chip choice, and the requirements any other candidate
must meet: [cc2652-radio-assessment.md](cc2652-radio-assessment.md).

**MPM fixes found along the way:**

| Issue | State |
|---|---|
| E010R5 changes the channel while the last packet is still on air (1 in 6 corrupted) | Fixed: [PR #1197](https://github.com/pascallanger/DIY-Multiprotocol-TX-Module/pull/1197) |
| XN297 emulation: no CRC xorout on TX in the unscrambled enhanced mode (RX applies it in debug builds) | Fixed, branch `xn297-enhanced-xorout` of the fork (also in `mpm-bench`). No protocol uses the mode, no PR |
| E01X: `HS6200_calc_crc()` reads `msg[pos+1]`, a stale stack byte: the CRC is right for 3% of E012 packets | Documented, with a fix (`msg[pos]`) checked against the CC2652. Needs an E01X model to test before a PR |
| Bench firmware only: CC2500 RX read `RXBYTES` before `MARCSTATE`, a packet ending in between came out short | Fixed in `mpm-bench` |
| Bench firmware only: `Serial.write()` blocked up to 23 ms on a slow host, the chips missed packets meanwhile | Fixed in `mpm-bench`: output through a RAM ring |

## Where things are

| Repository, branch | Content |
|---|---|
| [raphaelcoeffic/mpmv2](https://github.com/raphaelcoeffic/mpmv2) `main` | `src/radio.c`, `src/frame.c`, `src/bench.c` (`-DBENCH=ON`), `tools/bench/` (rig), `tests/frame/` (unit tests), `docs/` |
| [raphaelcoeffic/DIY-Multiprotocol-TX-Module](https://github.com/raphaelcoeffic/DIY-Multiprotocol-TX-Module) `mpm-bench` | MPM bench firmware (`_Bench.ino`, `_Bench_User.ino`, `_Bench_Config.h`, hooks in `Multiprotocol.ino`, `Validate.h`, `_Config.h`), plus the two fixes below |
| same, `xn297-enhanced-xorout` | The XN297 emulation fix alone |
| same, `e010r5-hop-fix` | The E010R5 fix alone, PR #1197 |

## Losses still to explain

Results below 100% with no cause found yet, most useful first. The others
(NRF address `5555555555`, 2 Mbps RX, the CC2500 and CYRF6936 losses on the
MPM v2 board, the MPM bench output stalls, FlySky in Wi-Fi channel 1, E01X
and E010R5) are explained in
[interop-results.md](interop-results.md).

1. **A7105 at 500 kbps, CC2652 -> A7105:** WFLY2 96.5 to 97.8%, Joysway
   94.4 to 98.6%, FlySky 99 to 99.8%, all CRC errors. Not Wi-Fi, power,
   frequency offset or GFSK vs 2-FSK; Joysway's errors depend on the data
   and on its RX table ([details](interop-results.md#a7105)). An SDR view of
   both chips shows the CC2652 signal clean, with a wider eye than the
   A7105's own: the errors are on the A7105 receiver side. Next: a real
   FlySky / AFHDS2A receiver (FS-iA6B).
2. **CYRF6936, CC2652 -> CYRF6936:** with the bench output fix all 100%
   but Kyosho3 (99%, single misses at a 10 ms period). MLINK is only received
   with the chips inverted (not understood).
3. **NRF24L01 2 Mbps, CC2652 -> NRF:** 92 to 97% (LaunchPad); MPM v2 97
   to 98.2% at ch 30 to 41, 99.2 to 99.8% at ch 2 and 80, while 1 Mbps gets
   100% at ch 40. Random losses: not Wi-Fi beacons, frequency offset, payload,
   address or deviation ([details](interop-results.md#2-mbps-cc2652---nrf)).
   The CFlie direction.
4. **Smaller ones:** Height 5 ch 88 to 100% (the A7105 RX restart between
   short packets?), Bugs 97.6 to 99%, NRF address `0f0f0f0f0f` 97.6 to 99.6%
   at 1 Mbps (a weak sync word). The LaunchPad CC2500 runs that dropped to
   91 to 100% on some re-runs (Skyartec, SFHSS, Corona) were most likely the
   bench output stalls (fixed); not re-run on the LaunchPad.

The loss timing tells Wi-Fi from a format problem: Wi-Fi beacons take out
packets 102.4 ms apart.

## Not covered yet

- **Sensitivity, range, blocking.** Nothing was measured: there is no
  attenuator on the bench. The signal is around -20 to -30 dBm from the
  LaunchPad, stronger from the MPM v2 (+20 dBm). At about -50 dBm (the
  CYRF6936 bind power) Wi-Fi already costs 1 to 17% depending on the channel;
  the CYRF6936 loses as much from the MPM v2 at its lowest power. These are
  the most likely losses:
  - the stricter sync threshold used for weak sync words;
  - the CYRF6936 DSSS processing gain, only partly kept: the chips are
    received as plain GFSK bits and correlated in software;
  - hard-decision Viterbi for the CC2500 FEC.

  The 0.2 to 2% of CRC errors from the CC2652 into the A7105 at 500 kbps have
  no reference to compare with.
- **Real receivers and models.** Every test was chip to chip, against the
  MPM's own chips. Bind, telemetry and the timing of the real protocols with
  real receivers are untested. The E01X CRC question needs a model.
- **Timing.** Only the ESB ack turnaround was measured (a 150 us window is
  enough at 1 Mbps). Still to measure, against the MPM protocol periods:
  - the channel change time (RF core `CMD_FS`) and the TX start latency;
  - the hop rates of the fast protocols;
  - the RX to TX turnaround of the telemetry protocols.
- **RSSI and LBT.** No test yet of RSSI accuracy for telemetry, of LQI, or of
  the carrier sense that FrSky LBT needs (the CC2500 CCA, `CMD_PROP_CS` on the
  CC2652).
- **2 Mbps RX.** The only route on the CC2652 is the BLE 2M PHY, untried (see
  the assessment).
- **TX power and the front end.** The MPM v2 board has an external PA/LNA
  (SKY66112, about +20 dBm). Output power, harmonics and the LNA's effect on
  sensitivity and blocking are not measured (no RF equipment yet).

## Next steps toward MPM v2

1. **Choose the chip.** Check the alternatives against the requirements list
   in the assessment. The CC2652's real gaps are 2 Mbps RX (CFlie) and how
   little of its RF core is documented. The next candidate is the CC2745R10
   (LaunchPad LP-EM-CC2745R10-Q1). Its radio does BLE 2M, so its demodulator
   runs at 2 Mbps, and a P variant has a +20 dBm PA. Its radio core is new
   (LRF, closed modem images): `radio.c` would be rewritten, while `frame.c`,
   `bench.c` and the rig carry over. The checklist is in the assessment.
   Measure sensitivity on the chosen candidate. The rig works with an
   attenuator in the path: `link.py --count 1000` prints the packet rate.
2. **Emulate the chips, not the protocols.** The MPM protocols drive their
   chips through register writes and FIFO accesses (`CC2500_WriteReg()`,
   `NRF24L01_WriteReg()`, `A7105_WriteReg()`, `CYRF_WriteRegister()`...). A
   "virtual chip" layer would keep a shadow of each chip's registers and turn
   them into a `radio_phy_t` and a `frame_t` when the protocol strobes TX or
   RX. It would also emulate the FIFO and the status the protocols read: RX
   length, CRC_OK, RSSI, LQI. The bench shows this mapping is mechanical:
   - `suite_cc2500.py --phy` derives a CC2652 PHY from any CC2500 register
     table;
   - A7105: `DATA_RATE`, `CODE_I` and ID;
   - CYRF6936: `TX_CFG`, `FRAMING_CFG` and the codes;
   - NRF24L01: `RF_SETUP`, `CONFIG`, `EN_AA`, `DYNPD` and the addresses.

   The protocol files (about 90) would then port with few changes and stay
   close to upstream. The other way, rewriting each protocol over a framing
   API, is cleaner but diverges from upstream. Recommended: the virtual chips,
   starting with the NRF24L01 / XN297 and CC2500 families (the most
   protocols).
3. **Port the MPM core.** Port the µs callback scheduler on a CC2652 timer,
   the protocol table, and the serial protocol from the radio (mpm-v2 already
   has the PPM input, input detection, the serial port and the flash). Then
   bring protocols over one family at a time, each checked on the bench
   (`proto_capture.py` and the suites work against the CC2652 too).
4. **Measure and fix the timing.** Measure RF core command latencies, then
   check that hopping and TX fit in each protocol's period. Telemetry and ESB
   need fast RX/TX turnaround.
5. **Test with real hardware.** Test each protocol with real receivers and
   models: bind, control, telemetry, failsafe. The E01X fix too.
6. **Hardware.** The MPM v2 prototype has the PA/LNA; the changes for the
   next revision are listed in [hardware.md](hardware.md). Its own frequency
   needs no per-board calibration (HPOSC correction). The receivers' crystals
   still vary: the 4-in-1 chips of the bench are 35 kHz (CC2500) and 90 kHz
   (A7105) off, which the MPM `option` corrects per model today.
7. **Update from the radio.** EdgeTX flashes the MULTI-Module over the
   module bay with an STK500 subset; the CC2652 ROM boot loader isn't on
   those pins, so the MPM v2 needs its own boot loader in flash. Proposal in
   [update.md](update.md).

## Questions for review

- Is the bench mode (`mpm-bench`) worth having upstream, for testing new
  hardware or new protocols?
- Does anyone have an E01X (E012 / E015) model to test the HS6200 CRC fix?
- Virtual chips or per-protocol rewrites: any preference, or constraints from
  how upstream evolves?
- Which protocols matter most for a first MPM v2 release?
