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
- **Chips between callbacks.** With more than 1.5 ms left, the chip
  emulations go on by themselves (`mpm_protocol_idle()`): the CYRF6936
  decodes a received packet then, not at the protocol's next register read.
- **No input.** 70 ms without a frame: the callbacks stop, and start again
  with the next frame (no restart of the protocol).
- **Model change.** A new protocol, sub protocol, rx_num or autobind flag
  restarts the protocol, with bind if the bind or autobind flag is set.
  Range check and low power don't. The bind flag set alone restarts with
  bind; cleared, the protocol's `end_bind()` is called (none: bind done).
- **Status telemetry** (`MP` 0x01, 24 bytes) at each protocol start, then
  every 500 ms, not for protocol 0 and not with telemetry disabled. Version
  2.3.<major>.<minor> of the project version (0.1.0: V2.3.0.1): major 2 for
  the MPM v2 port; minor 3 because EdgeTX only sends the frames' extra data
  (FrSky D16 bind option, S.Port, DSM, config) with minor >= 3, and checks
  the whole version against 1.3.3.0 ("module upgrade") and 1.3.3.30 (DSM
  clone). The protocols are those of MPM v1.3.4.74.
- **Protocol list** (protocol 0, `MP` 0x11): the entry given by the option,
  0xFF for the count, as EdgeTX scans it (`io/multi_protolist.cpp`).
- **Module id**: the chip's IEEE 802.15.4 address (last 32 bits), plus
  rx_num; `rx_tx_addr` as `set_rx_tx_addr()`.
- **LED**: the MPM blink patterns (no input, invalid protocol, bind).

**Links.** UART0 (LaunchPad: XDS110 back channel; MPM v2: USB console) and,
on the MPM v2, the module bay: UART1 in on the PPM pin (idle high: EdgeTX's
`ETX_Pol_Inverted`), telemetry out on S.Port through U9, idle low unless the
frame's `telem_invert` bit is clear (EdgeTX sets it for the external bay),
as the STM32 MULTI-Module's TX_INV. S.Port stays released until the first
frame from the bay, then idle 500 us before its first byte (out of the
released level, a break to the radio's idle low receiver, the first start
bit was missed: the protocol list's count, sent once, never reached EdgeTX
and its scan fell back to the built-in list). Telemetry goes back on the
link of the last frame. Tested on a TX15 (EdgeTX, 2026-10-21): frames decoded, the radio reads the
status ("Protocol invalid" for FlySky) and the protocol list scan (the
drop-down shows the ported protocols; a model on a protocol not in the list
shows its number: EdgeTX's `getProtoLabel()` after a scan). baysim 3 sets
each line's polarity as a radio does (before, it drove the PPM pin idle
low): see [update.md](update.md#link-polarity).

**PPM** (the bay only). The bay's input is detected on DIO22 with
`ppm.c`: GPT0 A captures the falling edges (24 bits at 48 MHz), and after
a gap of more than 2.2 ms the next 16 intervals are sampled by DMA. Four
channel widths in a row (800 to 2200 us) mean PPM, at most one interval
above 200 us means serial (UART1 then takes the pin), else detect again;
after 1 s without a frame (a MULTI frame that decodes, or a PPM frame) the
input is detected again. A PPM frame ends 2.4 ms after its last channel
pulse; it needs 4 to 16 channels after a sync gap, and a pulse below
800 us drops the frame until the next sync (the MPM's `bad_frame`: no
shifted channels). The core takes it as a serial frame with the protocol of
one `PPM_prot[]` entry (`mpm_ppm`, the MPM v2 has no dial): the build's
`MPM_PPM_PROTOCOL` (64, FrSky X2), `MPM_PPM_SUB_PROTOCOL`, `MPM_PPM_RX_NUM`,
`MPM_PPM_OPTION`, or `ppm.txt` ([Settings](#settings)). Widths scaled as
the MPM's `TX_ER9X`, EdgeTX's 1500 +- 512 us for +-100% (988 -> 204,
2012 -> 1844; other widths in `ppm.txt`), clamped to 0..2047; missing
channels at the center. No telemetry (S.Port stays released), no bind
other than autobind, no failsafe, no channel order in PPM mode. Tested with baysim's PPM
([update.md](update.md#tested-over-the-module-bay)): 4 to 16 channels,
both polarities, the widths to the microsecond over 500 reads (30 counter
wraps), the X8R linked (TX_LQI 100), PPM to serial and back in about 1 s.

On a TX15 (EdgeTX 2.12, external module PPM, 2026-10-21, J-Link reads of
the values and the debug log): PPM detected 76 ms after the application
starts, with either polarity ("−" idles low on the bay, "+" high; the
falling edges give the same intervals), and kept through a polarity change
while running. Sticks over the whole 988 to 2012 us on their channels,
1 us of jitter at rest (100 reads); 8 to 16 channels followed one at a time,
channel 16 following a switch; below 4 channels the input is lost, as
expected. **Frame length:** EdgeTX sends the channels, then a 3 ms tail,
and starts a frame every frame length (12.5 to 40 ms, whatever the channel
count); a start during the tail cuts it short. With 8 channels at 12.5 ms
and the sticks centred, the gap after the last channel was ~1.5 ms: no frame
end, the input lost (detected as PPM, then "no input" every second); at
15 ms it depends on the sticks (lost for a channel total between ~12.6 and
15 ms); at full deflection the next start is skipped (the DMA still runs)
and frames come every 25 ms. Safe: channels x 2.012 ms + 3 ms (19.1 ms for
8 channels), EdgeTX's default 22.5 ms. A receiver needs a sync gap of about
3 ms too: an EdgeTX setting problem
([EdgeTX #7928](https://github.com/EdgeTX/edgetx/issues/7928)), not
something a shorter end of frame could fix (a channel is up to 2.2 ms).
With short gaps two frames can also read as one (a 16 channel frame once,
the first 8 channels right).

Not there yet: PPM telemetry and the bind button in PPM mode, channel order remapping (`disable_ch_map` is decoded only), the telemetry
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
| `iface_nrf24l01.h`, `nrf24l01.c` | NRF24L01 emulation (`NRF24l01_SPI.ino`): the registers become `frame.c`'s NRF framing (ShockBurst, or Enhanced ShockBurst with EN_AA / DYNPD) and the RF core PHY (1 Mbps, 2 Mbps, 250 kbps); a payload goes on air when written in TX mode, pipe 0 RX into a 3 payload FIFO. Not emulated: acks and retransmissions (TX_DS once sent, MAX_RT never), ack payloads, pipes 1 to 5 |
| `V2X2_nrf24l01.c` | V2x2: Std, JXD506, MR101 |
| `XK_ccnrf.c` | XK (X450, X420, Cars) and MoFly, on the XN297 emulation |
| `iface_cc2500.h`, `cc2500.c` | CC2500 emulation (`CC2500_SPI.ino`), see below |
| `frsky.c` | FrSky D, V and X (`FrSkyDVX_Common`, `FrSkyD/V/X_cc2500`) |
| `iface_a7105.h`, `a7105.c` | A7105 emulation (`A7105_SPI.ino`), see below |
| `FlySky_a7105.c` | FlySky (AFHDS): Std, V9x9, V6x6, V912, CX20 |
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
  chip errors (`frame_cyrf_rx_count()`), in an RX of the maximum length.
  With a length field (8DR with SOP) the RX ends with the packet, as on the
  chip: the RF core interrupt decodes the length symbol and sets the length
  (`frame_rx_var_len()`); a length over the 16 byte buffer drops the packet
  and RX goes on. The MFG ID is the module id and 2
  zero bytes (a DSM receiver bound to an MPM v1 must be bound again). Not
  done: GFSK and DDR modes. PA_SETTING -5 dBm and up: default power.
  Timing: the chip sends at TX GO, the RF core about 430 µs later, the same
  for every packet: `frame_init()` (2.3 ms, its RX sync word search) cached
  for the last 16 codes (DSM: 5 rows, for TX and RX), the synthesizer tuned
  at the CHANNEL write, the RX-only sync threshold not setting up the PHY at
  TX. Before, 3.2 or 3.9 ms
  alternately: DSM's CH2 packet 0.5 ms late after CH1, a Lemon-RX DSMP
  received a quarter of the packets. Without a length field (the DSM bind
  response) a packet in progress keeps its RX through an abort: the DSM bind
  check restarts RX every 7 ms, shorter than the RF core's fixed length RX.
  32 and 64 chip codes: the chip receives both in 8DR whatever
  DATA_CODE_LENGTH, the SOP tells them apart (its second copy inverted with
  64 chips). A Lemon-RX DSMP answers the 64 chip DSM data with 32 chip
  telemetry, which the MPM receives. With a length field the RX syncs on the
  header bytes both share (`frame_cyrf_rx_alt()`), and the received second
  SOP copy picks the code length (`frame_cyrf_rx_is_alt()`), for the length
  symbol and the decoding: the Lemon's telemetry ends after 85 bytes
  (0.68 ms), not the 161 of 16 bytes in 64 chips. Decoding a DSM telemetry
  packet takes about 0.7 ms (each symbol against a table of the 128 rotated
  codes, built again when the codes change): done between
  callbacks, before the DSM read 4.4 ms after RX GO. Decoded at that read
  instead (4.6 to 9 ms then), the next DSM packets went out late.

The EEPROM (`eeprom_read_byte()`: AFHDS2A RX ids, clone ids, the CYRF ID) is
a file in the external flash: see [Storage](#storage).

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
| FlySky (1) | not yet compared (`proto_compare.py flysky`) | real A7105, see below |
| AFHDS2A (28) | same payloads, 7701 µs | same, 61.6 ms between visits of the channel |
| DSM (6, DSMX 11 ms) | same payload, 10001 µs | same, 253 ms between packets of the channel's CRC seed |
| V2x2 (5, Std) | same payload, 4000 µs, both packets of a channel visit (port at -72 dBm: a few lost, bind power) | same payload, 4005 µs (MPM 4002) |
| XK (62, X450 / X420 / Cars) | same payload, 4000 µs | same payload, 4000 µs (MPM 4002) |

XK: also the other way round, the port on the MPM v2 (driven by
`radio_sim.py -p v2` over its USB) received by the 4-in-1's nRF24L01 (X420,
Cars: 1 Mbps) or CC2500 (X450: 250 kbps), the 4-in-1 then given the port's
GID (`id 0x00f3be8e`): same bind and data payloads on all three. X450 and
X420 autobind for 3 s, Cars binds only when asked.

FlySky, not yet against the MPM's own FlySky: the MPM v2 in a TX15's bay
(EdgeTX 2.12) received by the 4-in-1's A7105 (bench, 1 m) on each of its 16
hop channels, 1 s each: 30 to 44 packets with a good CRC (0 to 4 bad), the
module id, the radio's channels, one packet every 24.2 ms on a channel (16 x
1.51 ms). The FlySky table only transmits: as a receiver its MODE_CTRL
(0x01) 0x42 leaves AIF (bit 5) clear, the A7105 then listens an IF (500 kHz)
off and the packets show up 600 kHz below; with `w 0x01 62` (as AFHDS2A)
from -300 to +100 kHz, as AFHDS2A's (`suite_a7105.py` receives with the
AFHDS2A table for this reason).

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
width and period each second, gaps; or SBUS (`watch --in2 sbus`): frames
each second, CH1-4, the frame lost and failsafe flags. `dsm_receiver.py` binds a DSM receiver
on it from the port or the 4-in-1 (`bind --sub N`) and cuts the signal
(`loss`). `frsky_receiver.py` drives a bound FrSky receiver from either
(`rate`, `loss`, `cold`), on SBUS or a servo output.

`radio_sim.py sport --proto 64 --sub <s>` plays a Lua script talking to a
bound FrSky X receiver (`sportTelemetryPush()`/`Pop()`): each request goes
in one frame's 8 extra bytes, as EdgeTX's `sendSport()` (no stuffing, no
CRC, held while the status reports the buffer low), and the replies come
back as S.Port telemetry. By default it reads the setup fields of Mike
Blandford's UNI receiver firmware (UNI-Setup7.lua: physical id 0x17, app id
0x0C20: version, mode, receiver type, tune offset...); `--push
PHYS:PRIM:APP:VALUE` (hex, repeatable) sends other frames, e.g.
`17:30:0c30:80` for the S6R/S8R stabilisation script. Over the bay:
`-p <baysim link port>`. The MULTI-Module has no S.Port polling any more
(only FrSky's own modules poll the radio as a sensor): the radio's frames
carry the data, the module's buffer-low flag holds them back.

| Receiver | Protocol | Result |
|---|---|---|
| FrSky V8R4-II (V8 mode) | FrSky V (25), option 0 | binds (F/S button held at power up), CH3 56 pulses/s, period 17.6 to 18.4 ms, as with the 4-in-1. Signal off: holds the last position (no failsafe set); back on, the new position after 0.14 to 0.56 s. Powered up with the port sending: the first pulse after 0.05 to 0.87 s (9 times); with the 4-in-1 (bench firmware) 0.05 to 39 s, whatever its option. 10 minutes (CH1-4 swept): no gap, 50 to 58 pulses each second. Right after the bind, no output for the first 90 s of tries (port and 4-in-1), not explained. No telemetry (receive only) |
| FrSky V8R4-II (D8 mode) | FrSky D (3), option 0 | binds (CH1-CH2 jumper at power up), CH3 55.5 pulses/s with the port and the 4-in-1; periods 18.0 ms or 17.3 / 18.7 ms in turn, from run to run and with the frequency, with either (the receiver's framing). Signal off: holds; back on, one 1500 µs pulse then the new position after 0.5 to 1.0 s (the 4-in-1 too). 10 minutes (CH1-4 swept): no gap, 50 to 57 pulses each second. Receive only, no telemetry |
| FrSky X8R (ACCST v2, EU LBT firmware) | FrSky X2 (64) sub 2 | binds, CH1-4 on SBUS; telemetry: TX_LQI 99 to 100 (as the 4-in-1), RX RSSI about 85, A1 |
| FrSky X8R (UNI-RX 85, Mike Blandford's firmware: finds V1 / V2 and FCC / EU while binding) | FrSky X (15) sub 0 and 2, FrSky X2 (64) sub 0 and 2 | binds in all four (F/S held at power up, then the port's bind), SBUS 112 frames/s (one per 9 ms packet), CH1-4 as sent, no frame lost or failsafe flag; telemetry TX_LQI 100, RX RSSI 212 to 230, A1 93 to 95. Bound to the port (X2 FCC), the 4-in-1 drives it too (TX_LQI 99) at 1 m, not at 50 cm (all frames lost, whatever the option); the port works at options -12 to +24 (UNI follows the frequency). Signal off: SBUS goes on, the frame lost flag after 0.13 to 0.19 s, failsafe (CH1 at center) after 1.03 to 1.10 s, 70 ms later than with the 4-in-1 (the MPM stops 70 ms after its input); back on, the new position after 0.21 to 0.44 s (4-in-1: 0.27 to 0.34 s). Powered up with the sender on: the first frame without failsafe after 0.05 to 0.99 s (4-in-1: 0.05 to 0.78 s; 0.05 s: probably not reset by the 1.5 s off). 10 minutes (X2 FCC, CH1-8 at center): no frame lost or failsafe flag, 111.7 frames/s, 41 seconds with 100 to 103 frames read (the monitor's, the receiver flagged no loss); telemetry TX_LQI 100 |
| FlySky FS-iA6B | AFHDS2A (28) sub 2 (PWM,SBUS), option 128 | binds (until the receiver answers), CH1-4 on SBUS at its serial servo port; telemetry (option bit 7) about 80 frames/s: RX 4.85 V, error rate 1%, RSSI 60, noise 101, SNR 41 |
| Spektrum AR610C | DSM (6) sub 4 (Auto): bind response 0xA2 (DSMX 22 ms), 7 channels; subs 0 to 3 (DSM2 binds: response 0x01) | binds (bind plug in, power cycled by `servo_mon.py`), AUX1 46 pulses/s at 22 ms in all four modes as with the 4-in-1 (same bind responses to both). Signal off: AUX1 holds 1.1 s then stops (no failsafe set), as with the 4-in-1; back on, the new position after 0.3 to 1.6 s. 42 minutes (DSMX 22 ms, CH1-7 swept): no gap, 41 to 48 pulses each second; 29 seconds with a pulse 170 µs early or late (the 4-in-1 too), one 19.86 ms period. Once, the first bind after rewiring, an Auto response 0x01. During some of the port's binds (none of the 4-in-1's yet) the MPM v2 also gets other packets with a good CRC on the bind channel, up to 48 alike (`e308b5c2...`), origin unknown: `dsm_receiver.py` only takes responses starting with the inverted module id. No telemetry |
| Lemon-RX DSMP (7 channels) | DSM (6) sub 4 (Auto): bind response 0xB2 (DSMX 11 ms), 7 channels | binds (bind plug in, power cycled by `servo_mon.py`), AUX1 45 pulses/s at 22 ms as with the 4-in-1 (10 to 22 before the CYRF TX timing fix); telemetry (32 chip codes) 44 to 46 frames/s, one every other 11 ms frame (16 to 20 before the decoding between callbacks): QoS (frame loss A 90 to 96, RX 4.95 V), TM1000 (temperature), sensors 0x03, 0x0A, 0x12, 0x34, 0x40 |
| WLtoys K989 car (1/28, V2x2 receiver) | V2x2 (5) sub 0, rx_num 0 | as with the 4-in-1: binds when powered up during the bind (no bind button, a new ID: the car's own transmitter binds on the ID 0 hop table, not needed), LED solid; throttle on CH2 (low = forward; the first reverse after forward brakes, the next reverses), steering on CH4 (high = left), centred channels (byte 80) hold still; keeps the bind across a car power cycle (solid 1 s after power up); signal off: wheels stop at once, LED blinks, back as soon as the signal is. No telemetry |
| WLtoys 284131 car (1/28, XK receiver), on the MPM v2 driven over its USB (`car_drive.py`) | XK (62) sub 2 (Cars), rx_num 0 | binds when asked (car on, its transmitter off; Cars does not autobind), LED solid after the 3 s bind; throttle on CH2 (low = forward; reverse right after forward brakes, the next reverses), steering on CH4 (high = left), CH3 and CH5..16 at minimum (bytes 0, 10, 11, 14 at 00 as its transmitter sends); differences with its transmitter (trims 40 40 40 instead of 3b 40 00, low bits in bytes 12 / 13) do not matter; keeps the bind across a car power cycle; signal off: wheels stop at once, back as soon as the signal is. Its own transmitter (`xk_sniff.py`): bind address and channels as MPM's, GID c20745, data address and channel derived from the GID as `XK_initialize_txid()` does, steering in byte 1 and throttle in byte 2 (01..7f right / forward, 81..ff left / reverse), the THR D/R knob scales the throttle. No telemetry |

Option: the LaunchPad sends about 34 kHz above the 4-in-1's CC2500 (MPM v2
frequency estimates of both); option -24 moves it there. The option moves
both the same way, about 1.5 kHz a step (MPM v2: the port at -12 / 0 / +12
/ +24: -22 / -2 / +17 / +34 kHz; the 4-in-1 at 0 / +24: -36 / 0 kHz). The
V8R4-II and the X8R (its reply about 6 kHz above the 4-in-1) work at option
0 at 1 m. Bound to the port, the V8R4-II takes 16 to 20 s to take a 4-in-1
at option 0 (36 kHz lower). At 10 cm from the 4-in-1 it is overloaded:
nothing at the bind frequency, but works 70 kHz away; fine at 50 cm. The
X8R with UNI-RX needs 1 m from the 4-in-1 (at 50 cm: no frame). The
X8R takes neither FrSky X v1 sub protocol: its firmware was found by binding
the four X / X2 variants in turn from the 4-in-1 (bench `proto`), then
sending each with different servo positions.

## Storage

littlefs (`lib/littlefs`, v2.10) on the whole external NOR flash
(MX25R8035F, 1 MB, 256 blocks of 4 KB), mounted at power up
(`file_system_init()`, `src/lfs_driver.c`). Formatted only when there is no
file system (`LFS_ERR_CORRUPT`: blank or foreign data), not on a bus or
version error; a smaller one (the 256 KB of the 2025 tests) is grown to the
flash size. Caches of a flash page (256 bytes), no malloc
(`LFS_NO_MALLOC`, files opened with `lfs_file_opencfg()`). littlefs gives
power loss safety (a file is replaced at close: the old content or the new
one) and dynamic wear levelling (copy on write into free blocks, the
metadata moved after 500 erases); static data is not moved.

The NOR driver (`src/nor_flash.c`) splits writes at the 256 byte pages,
switches an MX25R to its high performance mode (JEDEC ID `C2 28`: 4 KB erase
40 ms typical instead of 58, page program 0.85 ms instead of 3.2), and reports
the program and erase failures of the security register (littlefs then
moves the data to another block).

The EEPROM (`src/eeprom.c`): a 2 KB RAM copy of the file `eeprom`, read at
power up. `eeprom_write()` changes the copy only (a byte written with its
value: nothing to do); `mpm_port_idle()`, after each `mpm_poll()`, writes
the file back as soon as it changed (a CYRF ID created, an AFHDS2A bind).
Without a file system: RAM only, as before.

**The protocol keeps running during flash work.** A flush is about one
erase and nine programs (55 ms). The flash erases and programs on its own
with CS released: the driver's waits and each read call `_flash_yield()`
(main.c, `nor_flash_set_yield()`), which runs `mpm_poll()` (the callbacks
and the input; `mpm_port_idle()` is not nested) until the next callback is
at least 1 ms away (`mpm_time_left()`), at most 20 ms: the next slice of
flash work (a 256 byte read and littlefs's processing of it, up to 0.65 ms
measured) ends before the callback. Without that wait, path lookups (each
file request reads the directory's metadata, about 40 reads) left
callbacks up to 0.9 ms late, and the FS-iA6B's telemetry fell from 86 to
40-80 frames/s (its error rate stayed at 1%: AFHDS2A's telemetry RX is the
sensitive part); with it, 40 to 80 µs, 345 µs at worst over a 64 KB upload
and read back. The flush writes a copy taken at its start, so a callback
writing the EEPROM meanwhile makes it dirty again. The protocols' chips are
the RF core, not SPI; the SX1281 shares the bus but the MPM core doesn't
use it.

Measured on the MPM v2 (2026-10-20, `build-mpm`, DSM X 11 ms over the bay,
counters read over J-Link, since removed):
- the CYRF ID created at the first DSM start, written (72 ms), back after a
  power cycle with no write;
- FS-iA6B (AFHDS2A sub 2, option 128) bound over the bay: its RX id
  (`e6eba341`) written at offset 50; module off 3 s and on, no bind: the
  receiver's telemetry back at once (82 to 88 frames/s, as before the
  cut); rx_num 1 (no id stored): no telemetry;
- AR610C (DSM Auto) bound over the bay: its response starts with the
  inverted module id of the stored CYRF ID (`d40c4171`); module off 3 s and
  on, no bind: driven again. The port's CYRF ID derives from the module id
  anyway: this checks the stored copy is read back, the AFHDS2A RX id is
  what the storage keeps;
- flushes in steady state (J-Link writes to the copy): 55 to 56 ms, worst
  callback 0.2 to 0.5 ms late (29 µs without a flush), 5 flushes;
- 50 power cuts at random times (baysim, 0.3 to 1.2 s on) while a test build
  rewrote the file back to back (867 writes, counter + its complement +
  1.7 KB derived from it, checked at each boot): every boot read a
  consistent file, the file system always mounted. The boot loader's window
  (about 0.3 s) left 10 boots too short to count themselves.

Wear: with a write per bind, the host test's 100000 writes cost 1.01
erases each, spread over all the blocks (the most erased 397 times, the mean
394); at 100000 cycles a sector, about 25 million binds.

No file is part of the firmware: the CC2652's 352 KB of internal flash hold
every protocol (the whole MPM fits the STM32's 128 KB), tables included,
and the firmware runs the same with a blank flash or none (the EEPROM in
RAM only). Files are for tracing and customising: the EEPROM, debug output,
settings, all optional.

### Files over USB

`tools/mpm_fs.py` reads and writes the files over USB-C while the MPM core
runs (`src/file_server.c`, protocol in `file_server.h`): `info`, `ls [-r]`,
`get`, `put`, `rm`, `mv`, `mkdir`, `eeprom`, `log` (the [debug
log](#debug-log)). For debugging: the `eeprom` file
decoded (`mpm_fs.py eeprom`, or `tools/mpm_eeprom.py FILE` for a copy or an
STM32 module's dump: Multiprotocol.h's layout, the entries set: CYRF ID,
module ID, RX ids per rx_num, FrSky / AFHDS2A / Bayang / DSM RX, clones;
`--all` lists the erased ones too), written back (binds moved to another
module; the module reads it at power up, and its RAM copy overwrites the
file at the next EEPROM write), the [settings](#settings).

- Requests: frames starting with `FS` on the USB link (100000 8E2, the
  MULTI link: no MULTI frame starts so), seq, op, payload, CRC-32 (zlib's);
  replies in telemetry frames of type 0x40 (not an MPM type, USB only).
  Served by `mpm_port_idle()`, like the EEPROM writes: the callbacks run in
  the flash waits. A request repeated with the same seq gets the same reply
  without running again: the tool sends again after 1 s without a reply.
- Uploads: 128 bytes per WRITE, in order, into `.upload`; at CLOSE the size
  and CRC-32 given at OPEN are checked, then `.upload` is renamed over the
  file (at once). A bad CRC, a short or abandoned upload, a power cut: the
  old file stays (`.upload` removed at power up).
- READ keeps the file open for the next READ (no path lookup and seek per
  128 bytes); closed by the requests that change the file system and before
  an EEPROM flush (`file_server_sync()`: a replaced file's old blocks are
  freed).
- MPM v2 over USB (2026-10-20), AFHDS2A to the FS-iA6B over the bay
  meanwhile: 64 KB random file up in 12.4 s (5.2 KB/s), back in 11.9 s,
  identical, no request sent again; the receiver's telemetry 84 to 89
  frames/s throughout (86 without transfers); the EEPROM file read back
  with the FS-iA6B's RX id at offset 50.

### Settings

Text files at the root, all optional (`src/settings.c`): without one, the
build's defaults. Read at power up and again when the file server replaces,
removes or renames one: applied at once, the protocol restarted if it
changed. A file's bad lines keep their default and are listed in an error
file next to it (removed when there are none); `mpm_fs.py put`, `mv` and
`rm` print what the module made of it. Only the first 1024 bytes are read.

`ppm.txt`, the PPM mode's model (`src/mpm/mpm_settings.h`):

```
# FS-iA6B, bound with rx_num 0
protocol = FlSky2A    # name of the protocol table (case and spaces ignored) or number
sub = PWM,SBUS        # sub protocol name or number; a protocol alone: the first
rx_num = 0            # 0 to 63
option = 128          # -128 to 127, or the byte 0 to 255
autobind = no         # bind at each start (also 0/1, off/on, false/true)
low_power = no
min_us = 988          # PPM widths for -100% and +100%
max_us = 2012
```

```
$ mpm_fs.py put ppm_bad.txt ppm.txt
ppm.txt: errors (the defaults for those lines):
line 3: rx_num: 0 to 63: 99
line 4: option: -128 to 255: 300
```

MPM v2 (2026-10-20), baysim's PPM on the bay (8 channels), J-Link reads of
`mpm_ppm` and the core's frame: DSM X 2F option 7 from `ppm.txt`, then a new
`ppm.txt` put while running: AFHDS2A at once (1000 us still read as 223),
the FS-iA6B (bound over serial, sub 2, option 128) answering, nothing from
it with PPM off; `rm ppm.txt`: FrSky X2 again; a file with bad values: their
lines in `ppm.err`, the rest applied.

`debug.txt`: `usb = on` sends the [debug log](#debug-log) on USB from power
up (default off).

### Debug log

For users without a debugger: `debug()` / `debugln()` (`src/debug.h`, also
the MPM's own `DEBUG_SERIAL` lines in the protocols) go to a 4 KB ring in
RAM (`src/log.c`), each line with the time since power up, always: the last
lines before a problem are there when someone looks. Sent on the USB link
as telemetry frames of type 0x41 (USB only, like the file replies), from
the oldest line kept, while `tools/mpm_fs.py log` runs (request `LOG`), or
from power up with `debug.txt`. A build with `-DDEBUG=ON` also writes them
to Segger RTT, as before.

```
$ mpm_fs.py log
   0.000 ## Boot completed ##
   0.000 [NOR flash]: JEDEC ID c2 28 14
   0.005 file system mounted
   0.013 ppm.txt: protocol 64 sub 0 rx_num 0 option 0, 0 errors
   0.018 debug.txt: usb off, 0 errors
   0.027 bay: serial
   0.037 input: serial frames
   0.038 FlSky2A (28) sub 2 PWM,SBUS rx_num 0 option -128
   3.077 input lost
   3.120 input: serial frames
   3.175 FrSkyX2 (64) sub 2 LBT(EU) rx_num 0 option 0, bind
   4.131 bind done
   4.161 0 pps
   4.306 Busy 39 16
   ...
   6.232 bay: PPM
   6.241 input: PPM, 4 channels
   6.286 FrSkyX2 (64) sub 0 D16 rx_num 0 option 0
```

The core's lines: the bay's input detected or lost, the input (serial
frames, PPM and its channel count), each protocol start (name, number, sub
protocol, rx_num, option, bind, low power; or not in the firmware, or no
such sub protocol), bind done, the settings read. The protocols': what the
MPM prints in its debug builds (FrSky hop table, received packets a second,
LBT busy channels, S.Port data from the radio; DSM bind details under
`DEBUG_BIND`).

A line costs 15 us ("98 pps") to 70 us (a protocol start), 8 us a hex byte
(CC2652, measured): debug lines stay out of the busy paths. Bytes lost
before they were sent show as `[N bytes lost]`. The frames go out between
callbacks when the UART's buffer has room (64 bytes kept for telemetry when
the radio is on USB). MPM v2 (2026-10-20): boot lines read after the boot,
`debug.txt` sending from power up without a request and nothing once
removed, a 64 KB upload and read back identical with the log sent
meanwhile (FrSky X2 over the bay).

## Test protocol

Protocol 127, "Test": NRF24L01 ShockBurst every 4 ms, CRC16, TX_ADDR
`32764d504d` ("MPMv2" on air), 30 bytes: sequence number, flags (bind,
range, low power, failsafe), rx_num, sub protocol, the 16 channels packed as
in the radio frames, and `micros()` at the callback. Sub protocols: 1 Mbps,
250 kbps. Option: the RF channel (0 to 125, else 40). Bind lasts 2 s.

## Tests

```sh
make -C tests/mpm      # host: frame format against the MPM decoder, core with a fake port
make -C tests/storage  # host: littlefs and the EEPROM file on a simulated NOR flash
```

`tests/mpm/core.c` runs the core against a simulated radio (frames every
7 ms, fake clock): protocol start, callback timing (first one 5 ms after the
first frame, then within 1 µs), status contents and period, input loss,
bind, range check, autobind, rx_num, failsafe frames, invalid protocol and
sub protocol, the protocol list scan, a 150 ms job in `mpm_port_idle()`
in slices started 1 ms or more before a callback (callbacks on time, frames
processed).

`tests/storage/storage.c` runs `lfs_driver.c` and `eeprom.c` on a simulated
NOR flash (programs clear bits, erases set 0xFF): format of a blank or
garbage flash, the 2025 file system grown with its file kept, a power cut
after each program and erase of a flush from 300 file system states (3002
cuts, some in metadata compactions: the old content each time, the next
flush fine), a failing block, a callback writing during the flush, wear over
100000 flushes. File server: uploads read back, empty files, retried
requests answered from the cache, bad frame CRC, bad content CRC, short
and out of order uploads (the old file kept), rename / remove / errors, an
EEPROM write during an upload, a listing over 6 replies, a power cut at each
program and erase of an upload (the old file, no `.upload` left).

On the LaunchPad (the `firmware` application, not the bench),
`tools/bench/radio_sim.py` plays the radio over the XDS110 back channel:

```sh
cmake -S . -B build-lp -G Ninja -DBOARD=CC2652R1_LAUNCHXL
ninja -C build-lp flash-firmware
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
