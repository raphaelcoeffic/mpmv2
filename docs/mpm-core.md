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
frames), protocol telemetry to the radio (`Telemetry.ino`).

## MPM protocol files

`src/mpm/upstream/`: protocol files of the MPM (v1.3.4.74) compiled as C,
with one line added (`#include "mpm_compat.h"`).

| File | Content |
|---|---|
| `multi_enums.h` | The protocol and sub protocol enums of `Multiprotocol.h`, unchanged |
| `mpm_config.h` | The protocols built, as `_Config.h` / `Validate.h` |
| `mpm_compat.h/c` | The globals and helpers of `Multiprotocol.ino`, `Multiprotocol.h`, `TX_Def.h` and `Convert.ino`; the MPM names of the core's state (`Channel_data`, `rx_tx_addr`, `option`, the `IS_*` flags) are macros on `mpm` |
| `iface_xn297.h`, `xn297.c` | The MPM's XN297 emulation (`XN297_EMU.ino`) on the CC2652: same functions, `frame.c` builds and decodes the frames |
| `Bayang_nrf24l01.c` | Bayang |

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

`tools/bench/proto_compare.py` checks a port against the MPM: the LaunchPad
(MPM core, driven by `radio_sim.py`'s frames) and the 4-in-1 (MPM bench
firmware, `id` command for the same module id) run the protocol with the
same channels, the MPM v2 board receives each in the bind phase and after.

| Protocol | Bind | Data |
|---|---|---|
| Bayang (sub 0) | same payload, 2000 µs | same payload, 8001 µs |

At bind power (about -59 dBm at the receiver) one or the other loses 0 to
16% of the bind packets from run to run, in bursts; at full power the port
loses about 1%. Bayang telemetry (XN297 RX) is not tested yet.

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
