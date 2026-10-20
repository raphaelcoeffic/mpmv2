# MULTI-Module core

`src/mpm/`: the part of the MPM that talks to the radio and runs the
protocols, in plain C. It follows `Multiprotocol.ino` of the MPM (`loop()`,
`Update_All()`, `protocol_init()`, `update_serial_data()`), `Telemetry.ino`
(`multi_send_status()`) and `Multi_Protos.ino` (`PROTOLIST_callback()`). It
is not a drop-in build of the MPM sources: protocols are ported to it.

| File | Content |
|---|---|
| `mpm_frame.c/h` | Serial frames from the radio (26 bytes V1, 27 to 36 bytes V2), encode and decode; telemetry header and status flags |
| `mpm.c/h` | Main loop, protocol start, bind, status telemetry, protocol list (protocol 0), LED |
| `mpm_port.h` | What the core needs from the board: time, frames in, telemetry out, LED, RF reset, module id |
| `mpm_port.c` | The board port: UART0 at 100000 baud 8E2 (LaunchPad: XDS110 back channel, MPM v2: USB console) |
| `protocols.c/h` | The protocol table |
| `proto_test.c` | Test protocol 127 |
| `upstream/` | Protocol files of the MPM, and what they need from it ([below](#mpm-protocol-files)) |

## Behaviour

As the MPM:

- **Callbacks.** A protocol's callback returns the microseconds until its
  next call; the deadline advances by that much (32 bit µs, no limit at
  32.7 ms as on the AVR / STM32 timer). The first call is 5 ms after the
  protocol starts. A late callback runs the next one right away.
- **Input between callbacks.** A frame from the radio is processed only if
  more than 900 µs are left before the next callback, or after 10 callbacks
  without that much time.
- **No input.** 70 ms without a frame: the callbacks stop, and start again
  with the next frame (no restart of the protocol).
- **Model change.** A new protocol, sub protocol, rx_num or autobind flag
  restarts the protocol, with bind if the bind or autobind flag is set.
  Range check and low power don't. The bind flag set alone restarts with
  bind; cleared, the protocol's `end_bind()` is called (none: bind done).
- **Status telemetry** (`MP` 0x01, 24 bytes) at each protocol start, then
  every 500 ms, not for protocol 0 and not with telemetry disabled. Version
  1.3.4.74: the MPM release whose link this follows (EdgeTX warns below
  1.3.3.0).
- **Protocol list** (protocol 0, `MP` 0x11): the entry given by the option,
  0xFF for the count, as EdgeTX scans it (`io/multi_protolist.cpp`).
- **Module id**: the chip's IEEE 802.15.4 address (last 32 bits), plus
  rx_num; `rx_tx_addr` as `set_rx_tx_addr()`.
- **LED**: the MPM blink patterns (no input, invalid protocol, bind).

Not there yet: PPM input, the module bay (UART1, inverted, `telem_invert`),
channel order remapping (`disable_ch_map` is decoded only), the telemetry
buffer level flag, wait-for-bind, the bind button, `MULTI_SYNC` (input sync
frames), the telemetry formats other than `MULTI_TELEMETRY` (`Telemetry.ino`:
FrSky hub, S.Port, AFHDS2A and DSM frames only).

## MPM protocol files

`src/mpm/upstream/`: protocol files of the MPM (v1.3.4.74) compiled as C,
with one line added (`#include "mpm_compat.h"`) and the few C++isms fixed
(default arguments: macros, marked `mpm-v2`). The protocols sharing static
functions are built as one file, as the MPM builds its `.ino` files.

| File | Content |
|---|---|
| `multi_enums.h` | The protocol and sub protocol enums of `Multiprotocol.h`, unchanged |
| `mpm_config.h` | The protocols built, as `_Config.h` / `Validate.h` |
| `mpm_compat.h/c` | The globals and helpers of `Multiprotocol.ino`, `Multiprotocol.h`, `TX_Def.h` and `Convert.ino`; the MPM names of the core's state (`Channel_data`, `rx_tx_addr`, `option`, the `IS_*` flags) are macros on `mpm` |
| `iface_xn297.h`, `xn297.c` | The MPM's XN297 emulation (`XN297_EMU.ino`) on the CC2652: same functions, `frame.c` builds and decodes the frames |
| `Bayang_nrf24l01.c` | Bayang |
| `iface_cc2500.h`, `cc2500.c` | CC2500 emulation (`CC2500_SPI.ino`), see below |
| `frsky.c` | FrSky D, V and X (`FrSkyDVX_Common`, `FrSkyD/V/X_cc2500`) |
| `iface_a7105.h`, `a7105.c` | A7105 emulation (`A7105_SPI.ino`), see below |
| `AFHDS2A_a7105.c` | AFHDS2A |
| `iface_cyrf6936.h`, `cyrf6936.c` | CYRF6936 emulation (`CYRF6936_SPI.ino`), see below |
| `dsm_all.c` | DSM (`DSM.c`, `DSM_cyrf6936.c`; macOS file names are case insensitive: not `dsm.c`) |
| `telemetry.c` | `Telemetry.ino`, `MULTI_TELEMETRY` part: the core calls `TelemetryUpdate()` when no status frame is due, frames written only with 48 bytes free in the UART buffer |

The protocol table entries follow the MPM's: sub protocol names with their
length byte first (`STR_SUBTYPE_*`), the callback returns `uint16_t`, an
`end_bind` hook (`end_bind_counter()`: what `BIND_DONE` does for the
protocols that count their bind packets). The core calls
`mpm_protocol_prepare()` before each protocol's init: what `protocol_init()`
resets (CRC polynomials, telemetry, `prev_power`). `option_override` (RF
tune for the XN297 at 250 kbps, as on the CC2500) goes to the status.

The XN297 emulation, as the NRF24L01 in the MPM:

- TX starts at `XN297_WritePayload()`, RX runs from
  `XN297_SetTxRxMode(RX_EN)` to the next mode change, also across
  `XN297_RFChannel()`; one packet is kept until read.
- RX length: the payload, 2 bytes for an enhanced packet control field,
  the CRC.
- Channel n: 2400 + n MHz. At 250 kbps the PHY of the CC2500 emulation
  (127 kHz deviation) and the option as its frequency offset
  (1.587 kHz a step).
- Power: the board default when bound, `RF_TX_POWER_LOW` (`0x04C6`, about
  25 dB lower) for bind and range check. `XN297_HoppingCalib()` does nothing.
- Weak RX addresses get the stricter sync threshold, as on the bench.

### Chip emulations

The CC2500, A7105 and CYRF6936 protocols drive the chip's registers as on
the MPM; the emulation keeps a copy of the registers and FIFOs and, at TX
or RX, turns them into the RF core setup (`radio_init()` only when the PHY
changes, and again after each chip reset: another protocol may have set the
radio up), the frequency and a `frame.c` framing. Status registers read as
the protocols read them.

- **CC2500**: rate, deviation (MSK as 2-FSK, bits inverted), RX bandwidth,
  preamble, sync word (2 or 4 bytes), CRC, CC2400 CRC, variable length,
  whitening, FEC; FREQ / CHANNR / CHANSPC / FSCTRL0 to the frequency; RX FIFO
  with address check, CRC autoflush, appended RSSI and LQI; MARCSTATE,
  RXBYTES, TXBYTES. Not done: SYNC_MODE 0 (`CC2500_250K_Init()`), infinite
  length. PATABLE -10 dBm and up is the board default power, below
  `RF_TX_POWER_LOW`. Timing: the RF core setup and `frame_init()` (about
  170 µs) run again only after a register change other than the frequency
  ones; the RF core starts sending about 0.2 ms after the call and gives whole
  packets where the CC2500 FIFO fills byte by byte, so an RXBYTES read waits
  for a packet ending within 500 µs (its length known). FrSky X2 LBT: the
  X8R's reply ends about 0.1 ms after the protocol reads RXBYTES without the
  wait, 0.3 ms with the setup each time.
- **A7105**: data rate, ID (2 or 4 bytes), CRC, FEC, preamble length, FIFO
  length; PLL registers to the frequency; MODE (TRER, CRCF), RSSI ADC. Not
  done: whitening. TX_TEST -11 dBm and up: default power.
- **CYRF6936**: 8DR and SDR, 32 / 64 chip codes, SOP, length field, CRC seed,
  TX_INV; channel n at 2400 + n MHz plus TX_OFFSET (DSM: 2401 + n);
  TX_CTRL / RX_CTRL GO, TX_IRQ_STATUS (TXC cleared on read), RX_IRQ_STATUS,
  RX_COUNT, RX_BUFFER, RSSI (count 21 at -60 dBm, 1.9 dB a count). Without a
  length field the packet ends after EOP_CTRL symbols over the DATA64_THOLD
  chip errors (`frame_cyrf_rx_count()`). The MFG ID is the module id and 2
  zero bytes (a DSM receiver bound to an MPM v1 must be bound again). Not
  done: GFSK and DDR modes. PA_SETTING -5 dBm and up: default power.
  Timing: the chip sends at TX GO, the RF core about 430 µs later, the same
  for every packet: `frame_init()` (2.3 ms, its RX sync word search) cached
  for the last 8 codes, the synthesizer tuned at the CHANNEL write, the
  RX-only sync threshold not setting up the PHY at TX. Before, 3.2 or 3.9 ms
  alternately: DSM's CH2 packet 0.5 ms late after CH1, a Lemon-RX DSMP
  received a quarter of the packets. Without a length field (the DSM bind
  response) a packet in progress keeps its RX through an abort: the DSM bind
  check restarts RX every 7 ms, shorter than the RF core's fixed length RX.
  32 and 64 chip codes: the chip receives both in 8DR whatever
  DATA_CODE_LENGTH, the SOP tells them apart (its second copy inverted with
  64 chips). A Lemon-RX DSMP answers the 64 chip DSM data with 32 chip
  telemetry, which the MPM receives. With a length field the RX syncs on the
  header bytes both share and decodes with either (`frame_cyrf_rx_alt()`).

The EEPROM (`eeprom_read_byte()`: AFHDS2A RX ids, clone ids, the CYRF ID) is
in RAM for now: lost at power off.

### Checks against the MPM

`tools/bench/proto_compare.py` checks a port against the MPM: the LaunchPad
(MPM core, driven by `radio_sim.py`'s frames) and the 4-in-1 (MPM bench
firmware, `id` command for the same module id) run the protocol with the
same channels, the MPM v2 board receives each in the bind phase and after.

| Protocol | Bind | Data |
|---|---|---|
| Bayang (sub 0) | same payload, 2000 µs | same payload, 8001 µs |
| FrSky D (3) | same payloads, 9005 µs | same (frame and telemetry counters masked) |
| FrSky X (15, D16 v1) | same payloads, 9001 µs | same (random and counter bytes masked, CRC checked) |
| FrSky X2 (64, sub 2 LBT) | same payloads, 9002 µs | same (data channel from `FrSkyX2_init_hop()`) |
| FrSky V (25) | same payloads; timing differs, see below | same (seed and CRC masked) |
| AFHDS2A (28) | same payloads, 7701 µs | same, 61.6 ms between visits of the channel |
| DSM (6, DSMX 11 ms) | same payload, 10001 µs | same, 253 ms between packets of the channel's CRC seed |

DSM: the bench `cyrfid` command gives the MPM the port's CYRF ID (RAM
only), the codes of the capture channel come from `DSM.c`. The SDR bind
packets have no CRC and last 8 ms: Wi-Fi beacons (every 102.4 ms) corrupt
some (2% at -50 dBm from the MPM, about 10% at -66 dBm from the port at bind
power); their checksums filter them. FrSky V bind: its 53460 µs callback
wraps the MPM's 16 bit timer (about 20.7 ms between packets on a real MPM,
8.6 ms on the bench firmware's scheduler), the port waits the full time.


At bind power (about -59 dBm at the receiver) one or the other loses 0 to
16% of the bind packets from run to run, in bursts; at full power the port
loses about 1%. Bayang telemetry (XN297 RX) is not tested yet.

### Real receivers

`radio_sim.py drive --proto <n> [--sub <s>] --bind --bind-time 60` binds
(the module must be binding before the receiver is powered up in bind mode),
then sweeps CH1 to CH4 (`--sweep-ch`) and prints the status flags and the FrSky link
telemetry (S.Port RSSI and battery, hub link frame, AFHDS2A sensors, DSM
telemetry and bind response) each second. DSM Auto (sub 4): the bind
response sets the sub protocol and channel count, as EdgeTX does.
`servo_mon.py` (an RP2040-Zero with MicroPython, `rp2040/main.py`) switches
the receiver supply (GP11) and measures a servo output (GP12): pulses,
width and period each second, gaps.

| Receiver | Protocol | Result |
|---|---|---|
| FrSky V8R4-II (V8 mode) | FrSky V (25), option 0 | binds, servos follow; no telemetry (receive only) |
| FrSky X8R (ACCST v2, EU LBT firmware) | FrSky X2 (64) sub 2 | binds, CH1-4 on SBUS; telemetry: TX_LQI 99 to 100 (as the 4-in-1), RX RSSI about 85, A1 |
| FlySky FS-iA6B | AFHDS2A (28) sub 2 (PWM,SBUS), option 128 | binds (until the receiver answers), CH1-4 on SBUS at its serial servo port; telemetry (option bit 7) about 80 frames/s: RX 4.85 V, error rate 1%, RSSI 60, noise 101, SNR 41 |
| Spektrum AR610C | DSM (6) sub 2 (DSMX 22 ms), option 6 (channels) | binds (receiver first in bind mode), servo on AUX1 follows (`--sweep-ch 6`); no telemetry |
| Lemon-RX DSMP (7 channels) | DSM (6) sub 4 (Auto): bind response 0xB2 (DSMX 11 ms), 7 channels | binds (bind plug in, power cycled by `servo_mon.py`), AUX1 45 pulses/s at 22 ms as with the 4-in-1 (10 to 22 before the CYRF TX timing fix); telemetry (32 chip codes) 16 to 20 frames/s of the about 23 it sends: QoS (frame loss A 90 to 96, RX 4.95 V), TM1000 (temperature), sensors 0x03, 0x0A, 0x12, 0x34, 0x40 |

Option: the LaunchPad sends about 34 kHz above the 4-in-1's CC2500 (MPM v2
frequency estimates of both); option -24 moves it there. The V8R4-II and
the X8R (its reply about 6 kHz above the 4-in-1) work at option 0 at 1 m. The
X8R takes neither FrSky X v1 sub protocol: its firmware was found by binding
the four X / X2 variants in turn from the 4-in-1 (bench `proto`), then
sending each with different servo positions.

## Test protocol

Protocol 127, "Test": NRF24L01 ShockBurst every 4 ms, CRC16, TX_ADDR
`32764d504d` ("MPMv2" on air), 30 bytes: sequence number, flags (bind,
range, low power, failsafe), rx_num, sub protocol, the 16 channels packed as
in the radio frames, and `micros()` at the callback. Sub protocols: 1 Mbps,
250 kbps. Option: the RF channel (0 to 125, else 40). Bind lasts 2 s.

## Tests

```sh
make -C tests/mpm    # host: frame format against the MPM decoder, core with a fake port
```

`tests/mpm/core.c` runs the core against a simulated radio (frames every
7 ms, fake clock): protocol start, callback timing (first one 5 ms after the
first frame, then within 1 µs), status contents and period, input loss,
bind, range check, autobind, rx_num, failsafe frames, invalid protocol and
sub protocol, the protocol list scan.

On the LaunchPad (firmware built without `BENCH`), `tools/bench/radio_sim.py`
plays the radio over the XDS110 back channel:

```sh
cmake -S . -B build -G Ninja -DBOARD_CC2652R1_LAUNCHXL=ON
ninja -C build
openocd -f board/ti_cc26x2_launchpad.cfg -c "program build/firmware.elf verify reset exit"
tools/bench/radio_sim.py status        # status frames, every 500 ms
tools/bench/radio_sim.py scan          # protocol list
tools/bench/radio_sim.py bind          # binding flag set, then cleared
tools/bench/radio_sim.py link          # test protocol -> 4-in-1 NRF24L01
```

Results: status every 500 ms (±8 ms, host side), the scan lists the test
protocol, bind sets and clears the flag. `link` at 1 Mbps and 250 kbps: 500
packets, 0 or 1 lost, all channels right, callback period 3996 to 4005 µs
while the frames from the radio and the telemetry are processed.

The driverlib's GCC versions of `CPUcpsid()`, `CPUcpsie()`,
`CPUprimask()` and `CPUbasepriGet()` returned their value through an asm
output operand in a naked function, which GCC doesn't support: at -O0 it
went to r3 instead of r0, `IntMasterDisable()` returned garbage and the
interrupts stayed off after the first masked section. Fixed in
`lib/ti/.../driverlib/cpu.c` (basic asm writing r0, as TI's clang version),
`CPUdelay()` and `CPUbasepriSet()` too.
