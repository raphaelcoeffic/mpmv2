# Interoperability results

Over-the-air tests between the CC2652 (the mpm-v2 bench) and the chips of a
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
| NRF -> CC2652, 2 Mbps, proprietary PHY | 0% (no 2 Mbps receiver) |
| NRF -> CC2652, 2 Mbps, Bluetooth LE 2M PHY (`preset nrf2mble`) | 100% ([2 Mbps RX](#2-mbps-rx-the-bluetooth-le-2m-phy)) |

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

### 2 Mbps, CC2652 -> NRF

On the MPM v2 (`losses.py nrf nrf2m`, address `e7e7e7e7e7`, 5 ms period),
2 Mbps loses packets in the middle of the band and 1 Mbps doesn't, in
interleaved runs:

| Channel | 2 Mbps | 1 Mbps |
|---|---|---|
| 2 | 99.3% | 98.9% |
| 40 | 97.1 to 98.2% | 99.9 to 100% |
| 80 | 99.2 to 99.8% | 99.4 to 100% |

A ch 30 to 50 sweep (missing per 500) puts the losses over ch 30 to 41
(3 to 18), ch 42 to 50 1 to 6. Every loss is a missing packet: the NRF drops
CRC failures without a word. What it is not:

- **Wi-Fi beacons:** 100 pairs of lost packets, 8% within one packet of a
  multiple of 102.4 ms (random: 10%).
- **Wi-Fi or other traffic in general**, as far as the ESP32-S3 SDR sees it:
  2440 MHz is busy 1.4% of the time, 2480 MHz 1.8 to 3%. And 2 Mbps packets
  are half as long as 1 Mbps ones, which get through.
- **The ESP32's LO:** the same losses with the SDR tuned to 2440 or 2470 MHz,
  idle or capturing.
- **Frequency offset:** `trim` 0, -35, -170 and -300 give the same losses.
  (The SDR put the CC2652 135 kHz above this NRF at `trim -35`, but with the
  carrier then taken as the burst's median frequency, up to 140 kHz off on
  these packets; from the eye center the LaunchPad is 15 to 20 kHz above the
  NRF at `trim 0`.)
- **Payload:** fixed payloads (no counter) with byte 2 at 0x00, 0x32, 0x51,
  0x64, 0x9b, 0xc0 all lose 1 to 4%.
- **Address:** `c3c3c3c3c3`, `6565656565`, `a1b2c3d4e5`, `0102030405`,
  `0f0f0f0f0f` alike.
- **Power:** at ch 40, 10 dB less (`pwr 0x04c6`) only goes from 2% to 3%; at
  ch 80, from 0.5% to 3.7%. So at full power ch 40 has a floor that ch 80
  doesn't.

`sdr_compare.py nrf2m`, from the eye center (midway between the mean
levels of ones and zeros, LaunchPad, ch 40):

| Sender | Runs | Isolated bits | Worst bit per packet |
|---|---|---|---|
| NRF24L01 | 318 kHz | 250 kHz (79%) | 206 kHz |
| CC2652 proprietary, 320 kHz | 325 kHz | 293 kHz (90%) | 233 kHz |
| CC2652 proprietary, 500 kHz | 504 kHz | 451 kHz (89%) | 391 kHz |
| CC2652 Bluetooth LE 2M | 502 kHz | 392 kHz (78%) | 346 kHz |

All CC2652 packets decode on the SDR (300 of them, CRC right), and its eye is
no narrower than the NRF's own. Its shape differs: isolated bits at 90% of
the runs (less Gaussian smoothing than the NRF and the Bluetooth PHY), the
same at every deviation; the Bluetooth "enhanced TX shape" override
(`0x000D8C73`) doesn't change it on the proprietary PHY. (Measured from the
burst's median frequency, as `sdr_compare.py` did before, the CC2652's worst
bit was 71 to 85 kHz: the median falls 140 kHz off the eye center on its
packets.)

**The SX1281 as the sender** (`--cc-extra "chip sx1281"`, 2 Mbps, modulation
index 0.35: 350 kHz, BT 0.5, 0 dBm into the PA), 750 packets per point,
interleaved with the CC2652:

| Channel | CC2652 | SX1281 |
|---|---|---|
| 32 | 742 | 739 |
| 36 | 740 | 750 |
| 40 | 730 | 730 |

The SX1281 loses the same packets the CC2652 does, all missing.

**A larger deviation, the Bluetooth LE 2M PHY:** in interleaved runs
(`losses.py nrf nrf2m --dir rev --cc-extra ...`, 250 packets per run, ch 40),
the deviation does matter, and the Bluetooth PHY's TX (`preset nrf2mble`,
500 kHz) loses nothing:

| CC2652 TX | LaunchPad | MPM v2 |
|---|---|---|
| Proprietary, 320 kHz (`nrf2m`) | 728/750 (97.1%) | 731/750 (97.5%) |
| Proprietary, 400 kHz | 734/750 | |
| Proprietary, 480 kHz | 744/750 | 741/750 |
| Proprietary, 500 kHz | 2244/2250 (99.7%) | 740/750 (98.7%) |
| Bluetooth LE 2M (`nrf2mble`) | 1749/1750 | 750/750 |

(The earlier runs, not interleaved, found 320 to 480 kHz alike.) Above about
512 kHz the proprietary setup fails (status 0x0400). Other channels,
LaunchPad, 500 per point:

| Channel | 320 kHz | 500 kHz | Bluetooth LE 2M |
|---|---|---|---|
| 2 | 99.2% | 99.4% | 500/500 |
| 36 | 96.6% | 99.2% | 500/500 |
| 80 | 99.2% | 99.4% | 500/500 |

So a 500 kHz deviation removes the mid-band excess, and the proprietary PHY
keeps a floor of about 0.5% on every channel that the Bluetooth PHY doesn't
have. What else was tried on the proprietary PHY: the preamble (2, 4 and 8
bytes alike at 500 kHz; 8 doesn't help 320 kHz; 1 byte gets 29%) and 2-FSK
instead of GFSK (same losses). Lost packets are random (not tied to the
counter byte), all missing.

`suite_nrf.py nrf2mble` (six addresses, CC2652 -> MPM): 1500/1500 from each
board, where `nrf2m` gets 94.8 to 98.4% in the same run. The Bluetooth LE 2M
PHY is the CC2652's 2 Mbps setup both ways.

### 2 Mbps RX: the Bluetooth LE 2M PHY

The CC2652's proprietary PHY receives nothing at 2 Mbps, but its Bluetooth LE
2M PHY does, and the RF core runs the proprietary packet commands on it: TI's
wBMS setting for the CC2652R (`cc2652r_ble_pg21/setting_wbms_2m.json` in the
CC13xx/CC26xx SDK radio configuration) is `CMD_BLE5_RADIO_SETUP` (2 Mbps for
the "non-BLE commands", `rf_patch_cpe_bt5`) followed by `CMD_PROP_TX_ADV` /
`CMD_PROP_RX_ADV` with a 32 bit sync word. `preset nrf2mble` does the same
(`radio_phy_t.ble_2m`), with the NRF framing unchanged. Found on the
LaunchPad, October 2026:

- **Bit order and whitening:** as set up, the sync word and every byte go
  LSB first (NRF packets were received, 248/250, once the sync word was
  reversed on 32 bits, with every byte reversed), and the packet after the
  sync word is whitened with the Bluetooth LFSR (x^7 + x^4 + 1) seeded 0x65
  (advertising channel 37), the same on every channel and every packet. Both
  come from one byte of the CPE parameter table, found by diffing the RF core
  with the proprietary whitening on and off: an override sets it as on the
  proprietary PHY (MSB first, no whitening), see
  [RF core notes](rf-core-notes.md#cpe-parameter-table).
- **Demodulator:** with the Bluetooth settings, every packet had bit errors,
  always 0 bits turned to 1 inside runs of zeros; `trim` from -800 to +150
  kHz gave 0 to 26%. The fix came from TI's NESB 2 Mbps PHY of the CC27xx
  (`nesb_1m_2m_cc27xx.json` in the LPF3 SDK), tuned for the nRF24L01. Its
  modem registers have the same names and the same fields as the CC2652's:
  TI's Bluetooth values decode the same on both (matched filter `DEMMAFI1` =
  0x7b20 on the CC2652 = C3C5 123, C2C6 32 on the CC27xx; TI's CC2652
  "symbol tracking" override 0x00F9 on `0x50D4` = the CC27xx `DEMMISC2`
  Bluetooth values). Register names by offset come from the Beyond BLE tools
  of Adam Batori and Robert Pafford (38C3, see
  [RF core notes](rf-core-notes.md#credits)); the debugger reads the modem
  registers (`0x40045000`) while the RF core runs. Two NESB values on top of
  the wBMS overrides do it:

  | Override | Value | Without it |
  |---|---|---|
  | `0x50D4` `DEMMISC2`: MAFCGAIN 1 (frequency offset tracking) | `0x15F9` | nothing received |
  | `0x5100..0x5108` `DEMMAFI0..2`: matched filter for index 0.32 (C4 124, BLE 193) | `0x0300`, `0x4F15`, `0x007C` | 97% |

  NESB's sync correlator values (`DEMC1BE0..2`) made no difference (500/500
  without them) and are left out.
- **Switching PHYs:** the TI patches copy their image once per boot and count
  on the retained RF core RAM. After the proprietary patch, the Bluetooth setup
  hung (status ACTIVE): `radio.c` now resets both patches' state before
  loading one.

Results (`losses.py nrf nrf2m --cc-extra "preset nrf2mble"`, `esb.py --rate
2mble`):

| Test | LaunchPad | MPM v2 |
|---|---|---|
| NRF -> CC2652, ch 2 / 40 / 80 | 1500/1500 each | 92 to 95.8 / 98.8 to 99.6 / 99 to 99.4% (500 per run; ch 40 flat over `trim` -200 to +200) |
| CC2652 -> NRF, ch 40 | 1000/1000 | 1000/1000 |
| CC2652 -> NRF, `suite_nrf.py nrf2mble`, 6 addresses | 1500/1500 | 1500/1500 |
| NRF -> CC2652, `suite_nrf.py nrf2mble`, 5 addresses (not `0102030405`) | 1250/1250 | 96.4 to 97.2% |
| NRF -> CC2652, `0102030405` | 26 to 35% | 26% |
| ESB, CC2652 PTX, acked on the 1st try | 250/250, no retries | 250/250, 7 retries |
| ESB ack payloads (4, 31 and 32 bytes) | 250/250 | 250/250 |

CC2652 -> NRF on this PHY (500 kHz deviation) loses nothing, where the
proprietary PHY (320 kHz) loses 2 to 3% at ch 30 to 41: confirmed in
interleaved runs ([above](#2-mbps-cc2652---nrf)).

Address `0102030405` (`05 04 03 02 01` on air, mostly zeros) is received
badly: CRC errors (mostly one bit, the same positions: byte 0 bit 1, byte 3)
and missing packets, from 1% at `trim +150` to 72% at `trim -150`. A sync
word that unbalanced pulls the frequency tracking (MAFCGAIN 1), it seems;
other `DEMMISC2` values (MAFCGAIN 1 to 3, other symbol tracking gains)
changed nothing. Not fixed. The same address gets 100% at 1 Mbps and 250
kbps on the proprietary PHY, without MAFC.

The proprietary setups ran unchanged after
this (`suite_cc2500.py`, `suite_a7105.py`, NRF 250 kbps and 1 Mbps, ESB 1
Mbps). Not measured: the sensitivity of either PHY (no attenuator).

### SX1281 (MPM v2)

The MPM v2's second radio, behind U3 (`chip sx1281`, `src/sx1281.c`): GFSK
packet mode, 125 kbps to 2 Mbps, used as a raw radio under `frame.c` like the
RF core (sync word up to 4 bytes, fixed length, CRC and whitening off).
October 2026, the NRF setups (`losses.py nrf`):

- **2 Mbps RX, which the CC2652 can't do:** NRF -> SX1281 at ch 2 / 40 / 80:
  96.8 / 98 / 96% on the first try.
- **1 Mbps and 250 kbps RX**, interleaved with the CC2652 at ch 80 (a lossy
  channel that day: the CC2652 had 100% there before): 1 Mbps 92 to 94.8%
  (CC2652 93.2 to 94.8%), 250 kbps 88 to 92% (CC2652 88.4 to 90%).
- **RX single mode, re-armed after each packet** (continuous mode can lock up,
  datasheet errata 16.1). The driver also checks the chip mode every
  millisecond and arms RX again if it left it (`stop` prints `sx_rearm` and
  `sx_busy`): 0 re-arms since the `micros()` fix below.
- **ESB acks** (`esb.py` with `CC_INIT="chip sx1281"`, SX1281 PTX, NRF PRX,
  250 packets): 1 Mbps and 2 Mbps acked 250/250 on the 1st try or after 1 to
  4 retries, NRF 250/250, ack payloads 250/250 distinct. From TX done to RX
  ready takes 85 us (`sx_turn`; the ESB PRX answers after 130 us): TX done
  on DIO1, auto FS on, SPI at 12 MHz, 4 us PA ramp. `sx1281_send_rx()` arms
  RX with the ack sync word and a time-out of the ack window.
- **The drop-outs were the firmware, not the chip:** `micros()` (`timer.c`)
  went back by 1 ms when it ran between the GPT2-A reload and its interrupt,
  so every `micros() - start > timeout` fired at once. The SX1281 driver
  waits on BUSY and on the IRQ flags with such time-outs: false BUSY and TX
  time-outs, RX given up, init failures (status 0x55). `micros()` now counts
  a pending reload itself; no failure since (thousands of packets each way).
  It affects every user of `micros()`, the RF core code too.
- **Frequency** (SDR, carriers relative to each other): the SX1281 is 13 to
  17 kHz from the MPM's NRF24L01; at 500 kbps it is close to the CC2652, so
  the A7105 setups take the same `trim`. The CC2652's carrier moves with the
  bit rate, about -20 kHz at 500 kbps, +47 kHz at 1 Mbps and +164 kHz at
  2 Mbps relative to the SX1281 (open: the SDR has no absolute reference).

The suites with the SX1281 instead of the CC2652 (`CC_INIT="chip sx1281"`,
same `trim` as the CC2652), October 2026, % received:

| Suite | Setup | MPM -> SX1281 | SX1281 -> MPM |
|---|---|---|---|
| A7105 | flysky, afhds2a, bugs, height, height8, pelikan, kyosho, kyoshohype, wfly2, joysway | 100 (all) | 97.5, 100, 96, 100, 100, 98, 99.5, 100, 88.5, 98 |
| CC2500 | hott, hitec, ikea | 100 | 100 |
| CC2500 | redpine | 91 | 100 |
| CYRF6936 | dsm | 100 | 100 |
| CYRF6936 | devo, j6pro, wk2x01, scorpio, kyosho3, traxxas, wfly | 95, 97, 96, 98, 96, 98, 93 | 100 (all) |
| NRF | 1 Mbps, 2 Mbps | about 95 (CC2652 the same that day), 93.6 to 97 | |

Not on the SX1281: bit rates below 125 kbps (hubsan, sfhss, frskyx and the
other slow CC2500 setups), skyartec (229 kbps: no SX1281 rate within 1%), and
frames over 255 bytes or without a fixed length (dsmbind, dsmbindresp,
mlink, losi). The A7105 TX losses are the A7105 receiver's narrow frequency
window, as with the CC2652 ([Frequency margin](#frequency-margin)).

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
| 2 Mbps, proprietary PHY | 0% (2 Mbps RX) | 100% |
| 2 Mbps, Bluetooth LE 2M PHY (`--rate 2mble`), with ack payloads | 100%, no retries | 100% |
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
xorout (a table from the XN297 dump until upstream #1199, computed since, as in
`src/frame.c`) is applied only by `XN297_ReadEnhancedPayload()` in XN297Dump
builds of the MPM: the branch `xn297-enhanced-xorout` applies it on TX and RX
in all builds. No MPM protocol
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

The CRC errors at 500 kbps, looked at with the MPM v2 (500 packets per test,
the MPM reports the bad packets with their data):

- Random in time (no 102.4 ms Wi-Fi beacon pattern, no 20 ms microwave oven
  pattern), mostly one wrong bit per packet, plus missing packets. The rate
  moves from run to run (WFLY2: 0.2 to 6%) and over the day (Joysway 94 to 99%
  in the morning, 67 to 87% in the afternoon, while Hubsan at 100 kbps stayed
  at 100%), more than with any of the changes below (`losses.py a7105 <name>
  --dir rev`).
- Not changed by: the CC2652 power (about +20, +17 and +1 dBm out), 2-FSK
  instead of GFSK (the A7105 itself sends 2-FSK: `TX_I` FS = 0).
- The frequency offset matters more than first measured (WFLY2, 250 packets
  per point, two interleaved rounds, a later day): trim 0 / 30 / 50 / 70 /
  90 / 110 / 135 / 180 kHz gave 60-64 / 82-84 / 84-86 / 92 / 94-98 / 93 /
  76-78 / 58-60%. About 20 kHz off the best point doubles the losses, and
  the best point moves from day to day (earlier: flat from 60 to 180 kHz).
  The A7105 crystal drifting with temperature would explain this and the
  morning / afternoon difference above; not measured yet. The SDR puts the
  CC2652 at trim 95 about 23 kHz above the A7105's own TX carrier, so the
  A7105 receives best a little above where it sends. MULTI has the same
  problem with real receivers: `FORCE_<PROTO>_TUNING` in `_Config.h` for
  each A7105 protocol (`A7105_AdjustLOBaseFreq`).
- AFHDS2A tolerates the offset far better (same day, two rounds): trim 0 /
  45 / 90 / 135 / 180 kHz gave 93-95 / 98-99 / 99.6 / 96-98 / 86%. Its table
  sets ETH to 3 bits (`0x20` 0x1E) and the packets carry FEC. In the WFLY2
  table at trim 0 and 180, ETH 3 turns most missing packets into CRC errors
  (trim 0: 113 good, 52 bad, 85 missing -> 166, 58, 26), while AFHDS2A's
  `0x1A` / `0x1B` 0xFF and `0x1C` 0x2A change nothing. So the A7105 makes
  more bit errors as the offset grows, whatever the table: AFHDS2A hides them
  with the ID tolerance and FEC, the protocols without FEC at 500 kbps (FlySky,
  WFLY2, Joysway) don't. An AFHDS2A link between two
  4-in-1 modules (trainer) is reported rock solid.
- The bench A7105 RX settings are the protocols' own tables, checked
  against the datasheet (WFLY2, trim 95 unless noted):
  - RF switch: `A7105_SetTxRxMode` drives GIO1 / GIO2 as constant outputs
    (function 11xx, level set by the invert bit), RX = GIO1 0, GIO2 1, as the
    real protocols. The re-arm (STANDBY, `PLL_I`, RX strobe) is theirs too.
  - Calibration read back after the runs: IF filter `0x22` 0x05 (FBCF 0,
    FB 5), VCO band `0x25` 0x34 (VBCF 0, band 4).
  - `CODE_II` (0x20) ETH, the ID error tolerance: WFLY2 writes 0x12 (0 bits),
    the datasheet recommends 1. ETH 1 to 3 (0x16 / 0x1A / 0x1E) about halves
    the missing packets; the single bit CRC errors stay.
  - `RX` (0x18) FC on (0x72): flat from trim 0 to 180 kHz but 82 to 94%. FC
    only works with the MCU reading `AC` (0x12, 0x13) and writing the PLL
    back (AMICCOM reference code), which no MULTI protocol does.
  - CC2652 deviation: 186 kHz 81-86%, 220 kHz 93-94%, 250 to 280 kHz 94-99%,
    320 kHz 87-88%, 360 kHz 71-73%, 400 kHz 48-50%. The 250 kHz preset is
    right.
  - CC2652 preamble: 8 and 16 bytes no better than 4.
  - Gain (`RX_GAIN_I`) and VCO band, earlier: lower gain is worse (not
    overload), forced bands no different.
  - Carrier spread from packet to packet on the MPM v2 (SDR, 14 and 22
    packets): A7105 9.1 kHz sd, CC2652 10.8 kHz, not the 16 to
    23 kHz of the earlier table below.
- Joysway: most errors are bit 4 of payload byte 0 (0xA5 -> 0xB5), the first
  0 after about 10 alternating bits (end of the ID 0x2A, then 1010 0100).
  Other first bytes (0x00, 0x3C, 0xFF) don't do it. WFLY2 sends the same ID
  and first byte without it: the Joysway table receives differently. It sets
  `RX` (0x18) RXSM to 00 and `RX_GAIN_IV` (0x1C) LHC to 11, where the
  datasheet says 11 and 01; 0x1C at 0x0A cut the errors in some runs, not in
  others.

The signals themselves, seen by an ESP32-S3 SDR (`sdr_compare.py`, the same
packet from both chips, all payloads decoded right from the SDR bits):

| Sender (500 kbps) | Bit rate | Deviation, runs | Isolated bits | Worst bit | Carrier sd |
|---|---|---|---|---|---|
| A7105, FlySky / WFLY2 / Joysway tables | 499.97 to 500.10 kbps | 188 to 208 kHz | 108 to 129% of it | 141 to 156 kHz | 4 to 10 kHz |
| CC2652 (250 kHz preset) | 500.01 to 500.02 kbps | 239 to 250 kHz | 93 to 100% | 168 to 206 kHz | 16 to 23 kHz |

- The A7105 overshoots on isolated bits (no Gaussian filter, `TX_I` FS = 0);
  the CC2652 is flat and has the wider eye.
- At the Joysway byte 0 bit 4 the CC2652 sends -249 kHz (worst packet -221),
  nothing special at the end of its sync word. The flip happens in the A7105
  receiver with the Joysway table.
- Deviation from the CC2652 (Joysway, 250 packets): 186 to 250 kHz all about
  97 to 99%, byte 0 errors included; 300 kHz worse (92%).
- The CC2652 carrier moves 16 to 23 kHz (sd) from packet to packet, the
  A7105 4 to 10 kHz. Not looked into yet.

So the CC2652 signal is not the problem; the remaining errors are the A7105
receiver with these tables, which a real receiver may or may not share. A real
receiver settles it for the protocols that matter (FlySky / AFHDS2A, an
FS-iA6B is at hand), AFHDS2A with its FEC.

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
| NRF24L01 2 Mbps | (no 2 Mbps RX) | ch 40: 97 to 98.2%, ch 2 and 80: 99.2 to 99.8%, see [2 Mbps](#2-mbps-cc2652---nrf) |
| XN297, all modes of `suite_nrf.py` | 100% | 100% |
| ESB 250 kbps (`esb.py`) | | 250/250 acked on the 1st try |
| CC2500, the 18 setups of `suite_cc2500.py` | 100% | 100% at full power after the bench output fix, Corona V2 91 to 98.5% (before: 80 to 100%), see below |
| A7105, the 11 setups of `suite_a7105.py` | 100% | 95 to 100% (Bugs 95.6%, WFLY2 95%, Joysway and Pelikan 98.2%) |
| CYRF6936, the 12 setups of `suite_cyrf.py` | 96 to 100% (LaunchPad: 99 to 100%), Wi-Fi, see below | 100%, Kyosho3 99% |

CC2500, MPM v2 -> MPM below 100% at full power at first: Redpine 80%, Hitec
93%, Ares and FrSky X LBT 94%, Redpine slow 96.5%, FrSky X, IKEA and Corona V2
97%. Causes, none on the MPM v2 side:

- **MPM bench output stalls.** Redpine (3 ms period) loses runs of exactly 10
  packets: the packet before the gap is read 33 ms late by the MPM, while the
  MPM v2 sends on time (`stop` reports `late_max`, under 10 us). The bench
  firmware printed each packet with `Serial.write()`, which waits for room in
  the 256 byte USB buffer when the host is slow to fetch it; the chips, armed
  for one packet ("rx 0" on the CC2500), miss everything sent meanwhile. The
  same 23 ms stalls took CYRF6936 packets in pairs (Kyosho3 at a 10 ms
  period). Fixed in `mpm-bench` (2590841, output through a RAM ring): the
  worst read is then 400 us late, and all 18 CC2500 setups get 100% at full
  power but Corona V2. Hitec and Ares, which had looked like overload (100% at
  -5 dBm, worse at +20 and -21 dBm), were these stalls too.
- **Corona V2** freezes the CC2500 AGC (see above): 91 to 98.5% depending on
  the MPM v2 power, best at `pwr 0x10c3`.
- **MPM bench race (FrSky X LBT).** The "other" packets are the first 10 of a
  run, each 3 bytes short (last payload byte and status missing).
  `bench_receive()` reads `RXBYTES` before checking `MARCSTATE`: when the
  packet ends between the two reads, the MPM takes the radio as idle with the
  old byte count and flushes the rest. Fixed in `mpm-bench` (a212e9a, reads
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

### Frequency margin

Every suite setup at trim offsets of -100 to +100 kHz around the nominal
value (CC2500 -35, A7105 90, CYRF6936 0), both ways (CC2500 and CYRF6936 100
packets, A7105 200; `suite_<family>.py --trim`). Offsets where at least 95%
arrive:

| Family | MPM -> MPM v2 (MPM v2 receiving) | MPM v2 -> MPM (MPM v2 sending) |
|---|---|---|
| CC2500 | -60..+30 to -60..+60 for most; FrSky V, FrSky X LBT, Ares -30..+30; Redpine slow -30..0; HoTT -100..+100 | -60..+30 to -100..+60; Corona V2 none (85 to 88% at every offset) |
| A7105 | -100..+100 (+60 for Pelikan) | Hubsan, Bugs, Height, Kyosho Hype -100..+100; Pelikan -100..+60; AFHDS2A, Kyosho -60..+60; FlySky 0..+30; Joysway 92 to 98% at every offset; WFLY2 none (59 to 90%) |
| CYRF6936 | -100..+100, but DSM bind and bind response 87 to 95% at every offset (PA 0, Wi-Fi, above) | -100..+100 |

- The CC2500 windows of about +-45 kHz are the protocols' own narrow filters,
  the same both ways: the original MPM needs its frequency tune option with
  real receivers for the same reason. Their centre sits about 15 to 20 kHz
  below the nominal -35 that day.
- The 500 kbps A7105 protocols without FEC are the narrow ones (FlySky here
  received with the AFHDS2A table, ETH 3 bits; WFLY2 and Joysway with their
  own). Joysway's losses don't depend on the offset (byte 0 bit 4, above).
- CYRF6936 (DSSS) and the A7105 at 100 to 250 kbps don't care about 100 kHz.

## Automatic frequency trim

The CC2652 modem estimates the frequency offset of each packet it receives
([rf-core-notes.md](rf-core-notes.md#frequency-estimate)): `@rx ... foff=<kHz>`,
the sender above the CC2652. The bench `autotrim <offset>` moves `trim` by
the mean of every 8 estimates (when 5 kHz or more off), so that the CC2652
sits at the sender's frequency + offset. From the MPM's packets it then knows
where to send back: what a protocol with telemetry, or a bind exchange,
would do with a real receiver, instead of a fixed tuning option.

Scale (`autotrim.py <family> <name> --sweep <trims>`, the estimate against
the trim, LaunchPad): -0.84 to -1.04 kHz per kHz of trim on NRF 1 Mbps /
250 kbps, Devo, DSM, FlySky, Hubsan, Bugs, FrSky D / X, Corona; Redpine slow
-0.53 (one packet per trim). Spread from packet to packet: up to 2.5 kHz. With a scale under 2
the steps still converge: 1 to 3 steps of 8 packets from trim 0.

Where the estimate puts the MPM chips (trim at estimate 0, LaunchPad):

| Chip | Setups | Trim |
|---|---|---|
| CC2500 | FrSky D, X, X LBT, V8, HoTT, Hitec, Redpine, Corona, Ares, SFHSS | -22 to -35 (nominal -35) |
| A7105 | 500 kbps (FlySky, AFHDS2A, Kyosho, Joysway, WFLY2) | 61 to 66 |
| A7105 | 100 / 250 kbps (Hubsan, Pelikan, Bugs) | 64 to 88 |
| NRF24L01 | 1 Mbps / 250 kbps / 2 Mbps (Bluetooth LE 2M PHY) | -22 / -9 / 0 |
| CYRF6936 | Devo / DSM | +5 / -33 |

CC2652 -> MPM after learning from 48 MPM packets, starting at trim 0
(`autotrim.py`, 200 or 250 packets):

| Setup | Fixed trim (nominal) | `autotrim 0` | `autotrim 35` (A7105) / `-20` (CC2500) |
|---|---|---|---|
| WFLY2, LaunchPad, 2 rounds (trim 0: 42 to 52%) | 92.4 to 96% | 88 to 90% (trim 65) | 98 to 99.6% (trim 98 to 100) |
| WFLY2, MPM v2, 2 rounds | 94.4 to 98.8% | 93.2 to 95.2% (trim 57) | 93.2 to 96.8% (trim 92) |
| FlySky, AFHDS2A, Kyosho, Joysway, Pelikan, Hubsan | 98 to 100% | 97 to 100% | 98 to 100% |
| CC2500, the 10 setups above | 100% | 100% | 99.5 to 100% |

- The A7105 receives best about 35 kHz above where it sends (offsets 0 / 15 /
  25 / 35 / 50 on WFLY2: 88-90 / 95 / 96-98 / 98-99.6 / 98%); the SDR had
  put the best point 23 kHz above the A7105's carrier ([A7105](#a7105)). `autotrim 35` lands on the hand-tuned trim 90 on
  both boards, a little above on the LaunchPad.
- The CC2500 windows are wide enough for any of these trims (+-45 kHz): its
  window centre, 15 to 20 kHz below the nominal trim
  ([Frequency margin](#frequency-margin)), suggests `autotrim -20`.
- Learning needs packets: on the narrow PHYs (FrSky D) the CC2652 receives
  within about +-65 kHz only.
- Not measured: drift tracking over temperature (the A7105 crystal, the MPM v2
  HPOSC), the reason for doing this at run time.
