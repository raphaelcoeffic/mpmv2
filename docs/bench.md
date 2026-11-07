# Radio test bench

A test rig to check over-the-air interoperability between the CC2652 (mpm-v2)
and the chips of the current MULTI-Module (CC2500, NRF24L01 + XN297
emulation, A7105, CYRF6936). Both devices run a bench firmware that speaks the
same line protocol over a serial port, so one host tool drives both of them.
The results are in [interop-results.md](interop-results.md), the conclusions
for the chip choice in [cc2652-radio-assessment.md](cc2652-radio-assessment.md),
and the status and next steps in [status.md](status.md).

```
 host (macOS / Linux)
   tools/bench/*.py ──USB── 4-in-1 MULTI-Module, bench firmware    (MPM)
                    ──USB── CC2652R1 LaunchPad, mpm-v2 bench      (CC2652)
                              ↑ side by side, PCB antennas, about -20 to -30 dBm
```

## Hardware

- A 4-in-1 MULTI-Module with an STM32 and native USB. Ours: an iRangeX IRX4
  Lite (CC2500, NRF24L01, A7105, CYRF6936).
- A CC2652R1 LaunchPad (LAUNCHXL-CC26X2R1). Its XDS110 shows two serial ports;
  the first one (`/dev/cu.usbmodemL*1` on macOS) is the UART at 921600 baud.
- Both on the same desk, a few cm apart. No cables, no attenuator: the signal
  is about -20 to -30 dBm, far above sensitivity. Sensitivity and range are
  not covered by this bench.

**Module power and start:** USB power alone is enough, RF at full power
included (the IRX4 Lite takes nothing from an external 5V while USB is
plugged in). But at every power up its boot loader stays in DFU (USB
`1eaf:0003`, "Maple 003"), external 5V or not: it sees the USB host (D-,
PA11, low) and waits to be flashed. `tools/bench/mpm_start.py` starts the
firmware without writing anything (a DFU download request, then a USB reset
before the data reaches flash); a flash starts it too. The bench firmware
then shows up as a serial port (`1eaf:0004`, "Maple").

## Firmware

### MULTI-Module

The bench firmware is in the branch `mpm-bench` of
[raphaelcoeffic/DIY-Multiprotocol-TX-Module](https://github.com/raphaelcoeffic/DIY-Multiprotocol-TX-Module/tree/mpm-bench)
(`Multiprotocol/_Bench.ino`, `_Bench_User.ino`, `_Bench_Config.h`, enabled by
`MULTI_BENCH`). It needs an STM32 module and a debug build: the commands go
over the debug serial port. `_Bench_Config.h` keeps only the protocols the
bench uses so that a debug build fits in flash (88%).

`mpm-bench` also carries the two MPM fixes that change some results (each
also has its own branch on the fork, for upstream):

- `xn297-enhanced-xorout`: the XN297 emulation sends the unscrambled
  enhanced mode CRC without the xorout its RX side applies. Needed for
  `suite_nrf.py xn297enh_unscr*`, MPM -> CC2652. No MPM protocol uses this
  mode.
- `e010r5-hop-fix`, [PR #1197](https://github.com/pascallanger/DIY-Multiprotocol-TX-Module/pull/1197):
  E010R5 changes the channel before the last packet is sent; without it 1 in 6
  E010R5 packets is corrupted (`proto_capture.py e010r5`).

The bench output never blocks: it goes through a 2 KB ring, drained when USB
has room (`stop` reports `out_drop=<n>` if lines had to be dropped). Before,
`Serial.write()` waited on a slow host for up to 23 ms and the chips, armed
for one packet, missed the next ones. LED blinks on each packet sent, LED2
on each packet received.

```sh
git clone -b mpm-bench https://github.com/raphaelcoeffic/DIY-Multiprotocol-TX-Module.git
tools/bench/build_mpm.sh                # tools/bench/build/multi-bench.bin, board package toolchain
tools/bench/build_mpm.sh --native       # with the arm-none-eabi-gcc found in PATH
PORT=/dev/cu.usbmodem1201 tools/bench/build_mpm.sh --native --upload
```

`build_mpm.sh` looks for the checkout next to mpm-v2
(`../DIY-Multiprotocol-TX-Module`), or in `$MPM_DIR`. It needs `arduino-cli`
with the `multi4in1:STM32F1` board package. The FQBN is
`multi4in1:STM32F1:multistm32f103cb:debug_option=native` (`$FQBN` to change).
To go back to normal operation, flash a normal firmware.

### CC2652

```sh
cmake -S . -B build-lp -G Ninja -DBOARD=CC2652R1_LAUNCHXL
ninja -C build-lp flash-bench           # XDS110 (OpenOCD): boot loader and bench
```

### MPM v2

The same bench firmware runs on the MPM v2 board (`BOARD=MPM_V2`, the default).
Its RGB LED shows the RF activity: orange on TX, green on RX, red on a bad CRC,
dim blue when idle (LaunchPad: red on TX, green on RX, both on a bad CRC).
The console is the USB-C port (CH343, UART0 at 921600 baud), flashing goes
through the J-Link on J10:

```sh
cmake -S . -B build -G Ninja
ninja -C build flash-bench              # tools/flash_jlink.sh build/bootloader.elf build/bench.bin@0x2000
tools/bench/mpm_bench.py -p v2 shell
```

`tools/flash_jlink.sh` uses OpenOCD with the J-Link (`ti_cc26x2.cfg`): no
SEGGER DLL, so no J-Link EDU terms of use pop-up. `JLINK_TOOL=segger` uses
J-Link Commander instead (`$JLINK_DEVICE`, default `CC2652RB`). Both take
`$JLINK_SPEED` and `$JLINK_SERIAL`. `tools/serial_bl.py` only checks that the
ROM boot loader answers over USB (DTR/RTS drive RESET and BOOT); it doesn't
flash. The board itself is described in [hardware.md](hardware.md).

### Sonoff ZBDongle-P

The bench on a Sonoff Zigbee 3.0 USB Dongle Plus (CC2652P), as a FASST
sniffer (`fasstrx`): one image at 0x0 with the CCFG, no boot loader of
ours. The chip's ROM boot loader flashes it over the dongle's USB (CP2102N:
RTS on reset, DTR on BOOT, DIO15), as Sonoff's own flasher does. The CCFG
is Sonoff's (boot loader backdoor, 48 MHz crystal trim, forced VDDR,
DC/DC off), but for MODE_CONF VDDR_CAP (unused on the CC26x2). Console: the same USB port, UART0 (DIO12 / DIO13)
at 921600; `info` says `dev=cc2652p`. No LED, no `timing`, no SX1281; the
RF switch (DIO29) stays on the normal path, TX power words are the
CC2652R's (not measured).

```sh
cmake -S . -B build-zbp -G Ninja -DBOARD=ZBDONGLE_P
tools/rom_flash.py --sonoff --read zbdongle-p-stock.bin   # first: keep Sonoff's firmware
ninja -C build-zbp flash-bench          # tools/rom_flash.py --sonoff build-zbp/zbdongle-p-bench.bin
```

`tools/rom_flash.py --sonoff zbdongle-p-stock.bin` puts the saved Zigbee
firmware back. The host tools take the dongle's port as the CC2652's:
`CC_PORT=/dev/cu.usbserial-<id>` (or `-p`).

The MPM port runs on it too, in place of the bench, as on the CC2652
LaunchPad: `ninja -C build-zbp flash-firmware` (`zbdongle-p-firmware.bin`,
also one image at 0x0), the radio's link on the same USB port at 100000
8E2 (the CP2102N takes it on macOS): `radio_sim.py -p /dev/cu.usbserial-<id>
...`. The EEPROM is in RAM (`eeprom_ram.c`, lost at reset) and served as
the file `eeprom` ([mpm-core.md](mpm-core.md)); no PPM input, no module bay
(baysim needs the bay's pins). `flash-bench` puts the bench back.

**Sniffer page for helpers** (`tools/sniffer_web/index.html`): a static page
using Web Serial (Chrome, Edge, Firefox 151+, desktop) that loads the bench on the
dongle through the ROM boot loader (`rom_flash.py --sonoff` in JavaScript,
backup button included), then runs guided recordings and saves them as one
text file: `# key: value` header lines (system, mode, area), then per
recording `@step`, `@system`, `@note`, `@time` and the bench's lines. FASST:
`fasstrx 0 0 48` (stopped by a newline). T-FHSS: `preset tfhss`, `ch 45` (or
`ch 105`: channels n = 10 and 22, between the Wi-Fi channels), `rx 16 fixed`,
`stop`; its live count is the number of packets sharing the most common CRC
syndrome (CRC-16/MCRF4XX of the raw 12 bytes XOR the CRC field: constant for
one TX on one channel, the whitening key being fixed there). Two reference
systems count packets with `crc=ok`: S-FHSS (`preset sfhss`, `ch 100` or
`ch 190`, `rx 13`), a known protocol that checks a helper's setup, and Mini-Z
(`preset kyosho`, `ch 13` or `ch 140`, `rx 38`: the Kyosho FHSS bind packets,
ID 5475C52A on A7105 channels 0x0D and 0x8C, to tell whether the 4PM Plus's
Mini-Z Evo is that protocol; its data packets use the TX's own ID, so only
bind is heard). `ninja -C build-zbp sniffer-web` puts the page and the image
(`fasst-sniffer-zbdongle-p.bin`) in `build-zbp/sniffer-web/`; `wrangler deploy`
(`wrangler.jsonc`) publishes that folder as a Cloudflare Worker's static
assets (https://fasst-sniffer.edgetx.workers.dev). The page also lists the
XDS110, so recording can be tried on the CC2652 LaunchPad (no flashing
there).

### CC2755P10 LaunchPad

The bench (`src/core/bench.c`, `frame.c`, `main_bench.c` as on the MPM v2)
runs on the LP-EM-CC2755P10 over a `radio.h` of its own
(`src/platforms/cc27xx/radio.c`, `timer.c`, `uart.c`: the CC2652's file
names), UART on the XDS110 back channel at 921600, `info` says `dev=cc2755`. The suites run unchanged with
`CC_PORT=/dev/cu.usbmodemLS5600OQ1`:

```sh
cmake -B build-cc27 -G Ninja -DBOARD=CC2755P10_LP
ninja -C build-cc27 flash-cc27-bench
```

What differs from the CC2652 bench:

- every PHY is `phy_fsk` (below), GFSK BT 0.5 or, with the PHY's 2-FSK
  flag, a rectangular pulse (TX only: the RX is the same). `rx_bw`,
  `if_freq` and the sync threshold are ignored;
- `pwr`: the CC2652's PA table words taken as their dBm (+5 dBm at most),
  words below 0x100 as dBm (`pwr 10`, `pwr 0xf6` for -10), other words
  -20 dBm (`coronav2`'s 0x10c3);
- variable length RX receives the maximum length (the frame decoders cut);
  no CRC verdicts (all packets count as ok);
- RX at a multiple of the rate for a large deviation (`redpineslow`,
  below): packets of 255 / 4 - 3 bytes at most there;
- no `timing` (no test pins), no SX1281, `radio_hposc_offset()` 0;
- packets up to 255 bytes; the RX FIFO entry carries the frequency
  estimate (about deviation / 72 per step, the sender above is positive,
  compressed beyond about 60 steps), the RSSI and the sync time (SYSTIM,
  captured as TI's `RCL_Hal_setupSyncFoundCap()`).

The alarm is SYSTIM channel 0 on CPU IRQ 16, as TI's ClockP
(`kernel/nortos/dpl/ClockPLPF3_nortos.c`): the channel's event stays set
after the compare until the compare value is read, so ICLR alone gives an
interrupt storm. `sched` is 1 to 1.5 us late.

Against the 4-in-1 (`suite_cc2500.py --trim -70`, `suite_a7105.py`): the
CC2500 setups 100% both ways (`frskyx2eu` 99.5%; `redpineslow` since the
RX oversampling, below), except `coronav2` CC27 -> MPM (at -20 dBm,
250/250 at -10: the CC2500's frozen gain takes a window of levels, -15 to -5
dBm for the CC27 at the bench; [cc2755-vs-cc2652.md](cc2755-vs-cc2652.md)). In GFSK `ares`, `hott`, `hitec` (2-FSK on the CC2500) lost 5 to 30%
CC27 -> MPM, scattered single bit errors at -43 dBm; with the rectangular
pulse 250/250, as the CC2652. The A7105 setups (`ch 170`): 100% A7105 ->
CC27; CC27 -> A7105 at trim 70: 98 to 100%, `wfly2` 91% (97% at trim 20 to
40). The presets are GFSK; in the same run (500 packets, `ch 170`) the
CC2652 at trim 20: `flysky` 91%, `wfly2` 63% (the narrow window of
[interop-results.md](interop-results.md)), the CC27 at 70: 99%, 90%. In
2-FSK (`phy 500000 250000 97 4 0x8000 1` after the preset) the CC27's
`wfly2` gets 97%, the CC2652's no better (89%, 55%). Earlier CC27 runs gave
64 to 90% at trim 20: the window moves between runs. Trap: Wi-Fi on channel 1 here,
the suites' `ch 0` setups (2400 to 2410 MHz) lose a packet every 102 ms
(the beacons) and in bursts, whatever the sender: compare radios at
2440 MHz or so (`hitec.txt` turns the autocalibration off: `s 0x36;s 0x33`
after a channel change).

`suite_nrf.py` (the 4-in-1's NRF24L01, its CC2500 for the 250 kbps XN297
setups, ch 40), the CC27 at +20 dBm (`CC_INIT="pwr 20"`): CC27 -> 4-in-1
98 to 100% (`nrf2m`, 2 Mbps: 94 to 98%, as the CC2652); 4-in-1 -> CC27 90
to 100% (the CC2652: 97 to 98% on the same tests; `xn297enh_unscr` 89 to 93%,
the CC2652 98%). At +5 dBm the CC27 -> NRF24L01 links lose 4 to 6%, one
packet per 102 ms (Wi-Fi beacons at 2440 MHz): the MPM v2 sends 25 dB
stronger. `suite_cyrf.py` (ch 30, +20 dBm): `dsm`, `devo`, `j6pro`,
`wk2x01`, `scorpio`, `kyosho3`, `traxxas`, `wfly` 100% CC27 -> CYRF6936,
92 to 100% the other way. `dsmbind`, `dsmbindresp`, `mlink`, `losi` (SDR,
64 chips per bit: a 16 byte DSM bind packet is 1029 bytes on air) stream
through the PBE's FIFOs (`rf.c`, packets above 255 bytes, up to 1100): TX
refilled while the CPU polls (auto deallocation, `TXADEAL`), RX copied out
behind the PBE's write pointer (`RXFWP`, ahead of the commit) with the
FIFO's write limit (`RXFSRP`) moved after it, one packet per operation and
RX started again. 96 to 100% CC27 -> CYRF6936, 90 to 100% the other way
(they were 0%; the CC2652: 89 to 100%). The errors left are SDR decisions
(single 64-chip bits, a packet decoded inverted from its middle on): the
streamed copy matched the FIFO's last 260 bytes at the end of every packet
(200 packets checked). Trap: the bench must call `radio_receive()` /
`radio_rx_active()` within 2 ms at 1 Mbps (4 ms for TX), else the FIFO
overflows mid-packet. Same day: the 4-in-1's CC2500 needed `--trim -80` for
`frskyd`, `frskyv` (0% at the default -35 and at -40) and `radiolink` (70%
at -35), all three 100% at -80 (short packets: not streamed), CC27 -> A7105 `flysky` 81%, `wfly2` 55% (the firmware before:
83%, 55%): the 4-in-1's drift, not the streaming.
Trap: after the 4-in-1 restarted (USB hub power), its NRF24L01 sent the
right payload with a wrong CRC and received nothing until `info` (which
resets every chip) ran once.

Against the MPM v2 bench with CC2500 framing (`frskyd`, `frskyx`, `frskyv`,
`redpine`, `corona`, `hott`, length byte set): 100% both ways with the
MPM v2 at trim 0 to +50 (frskyx: nothing outside that; at -35, the trim for
the 4-in-1, nothing at all, which looked like a framing problem).

The TX pulse: LRF's TX filter is a symmetric FIR of 48 taps at 16 per
symbol (LRFDRFE DTX), `LRF_TxShape` its last 24 taps (TI's BT 0.5: a
Gaussian-filtered rectangle, 20 taps), scale 2^28 / the sum of the 48 taps
(17 bits). 2-FSK: 8 taps of 255.

`src/platforms/cc27xx/rf.c` drives TI's LRF layer (`lib/ti/drivers/rcl`, no RCL
scheduler) with raw framing: sync word, fixed length payload. The PHY
settings (`src/platforms/cc27xx/phy_nesb.c`, `phy_fsk250.c`) come from
`tools/cc27_phy.py`, the same registers as SysConfig's `ti_radio_config.c`
for TI's NESB PHY and its CC23xx MSK 250 kbps one (ported). Against the MPM v2's `preset nrf1m` /
`nrf2m` raw framing: 999 or 1000 packets of 1000 both ways at 1 and 2 Mbps.
What the generic PBE needs for raw RX:

- a CRC: with 0 CRC bits the PBE writes the packet to the FIFO, never
  commits it and hangs (op not even ended by a hard stop). `rf.c` receives
  the last byte as an 8-bit CRC, appended to the FIFO entry whatever the
  verdict;
- sync word B programmed (as A): unset, the MCE never starts its
  demodulator;
- NESB mode off (the NESB PHY turns it on);
- the trim fields written 0 in the setup: `LRF_setupRadio()` ORs the FCFG
  trims in.

TI's RCL-489 workaround (IF mixer sign, I and Q swapped on CC27xx) is right
for this chip.

TI has no CC27xx PHY below 1 Mbps; `cc27_phy.py --from cc23xx` ports a
CC23xx one (same LRF radio): fields by name, then the differences between
TI's NESB PHY of both families (CC27xx-only registers, analog settings,
PDIFIQCONJEN for the I/Q swap, the RSSI offset). Ported NESB 1 Mbps = TI's
CC27xx one. The MSK 250 kbps PHY (genfsk images) receives the MPM v2's
`hott` PHY (1000/1000), but its TX (MOD0.SYMSHP 2, fixed DTX taps, its own
symbol mapping) is rubbish to FSK receivers (the SDR sees a 1 Mbps-like
bit clock); `--gfsk` sends GFSK instead (NESB's BT 0.5 shape, SYMSHP 0,
NESB's MODSYMMAP0): `src/platforms/cc27xx/phy_fsk250.c`, both ways with `hott`
(196/200, 1000/1000); `nrf250k` and `xn250k` (160 and 127 kHz deviation)
are received at about 85% with it as is.

Other rates: `src/platforms/cc27xx/phy_fsk.c` (shell: `g RATE DEV [LOG2_PDIF]`) sets up
that PHY (above 1.2 Mbps TI's NESB 2 Mbps one: the genfsk images don't
transmit at 2 Mbps, TI's CC23xx 2 Mbps GFSK uses ble5 too), then changes
the rate word (BAUD/BAUDPRE, prescaler 12 at least: with 2 nothing is
received at 2 Mbps), the deviation (swConfig, LRF programs the TX shape) and
the decimation (DEMMISC3). The demodulator runs at 4 samples per symbol
whatever the rate: a fractional resampler (DEMFRAC P/Q, which
`LRF_programFrequency()` computes from BAUD and DEMMISC3) brings the
decimators' output (f_synth / 288 / BDE2) down to 4 x rate x PDIF
decimation. On the bench:

- BDE2 above 8, PDIF decimation above 2 and BDE1 decimating receive nothing
  (digital gains tried too); the resampler takes the rest (P/Q 0.2 at
  27 kbps is fine);
- PDIF decimation 2 when the samples' rate is short of twice the Carson
  bandwidth: it widens the channel (`nrf250k` 85% -> 100%), but with h = 0.5
  (`hott`) it loses packets;
- PDIF decimation slows the modulator too: TX runs without it
  (`rf_set_demmisc3()`).

Against the MPM v2's presets (raw framing, sync 71764129, -60 dBm), both
ways 99 to 100% (the MPM side's RX counts as with TI's 250 kbps PHY):
`frskyv`, `frskyd`, `radiolink`, `ares`, `frskyx`, `frskyx2`, `frskyxeu`,
`corona`, `hubsan` (100 kbps, 250 kHz deviation), `sfhss`, `skyartec`,
`hitec`, `hott`, `bugs` (250 kbps, 250 kHz), `nrf250k`, `xn250k`, `flysky`
(500 kbps), `nrf1m`, `nrf2mble`; the MPM's own 2 Mbps PHY (`nrf2m`) receives
none of the CC27's 2 Mbps packets, TI's NESB 2M ones neither. `redpineslow`
(10 kbps, 38 kHz deviation, h = 7.6) at the rate receives nothing: the
tones sit at the Nyquist limit of the samples (4 per symbol, PDIF
decimation 2 at most: 80 kHz), no room left for a frequency offset.
`phy_fsk` then runs the RX at a multiple of the rate (`rx_os`, 4 at most)
until half the samples' rate is the deviation + 60 kHz (only `redpineslow`:
4, the other presets have 79 kHz or more at 1). `radio.c` searches the sync
word's first 32 / `rx_os` bits with each bit repeated `rx_os` times (its
last 16 would match `d391d391`'s first half), receives the rest of it as
data, takes a majority vote per `rx_os` bits, checks the rest (1 bit error
at most) and drops it. The TX stays at the rate: `rf_set_modem()` keeps
BAUD, BAUDPRE and DEMMISC3 for TX and RX. `redpineslow`: 100% both ways
with the 4-in-1 and the MPM v2, window about 100 kHz (2 x: 65 kHz). The SDR measures the CC27's
deviation at 27-28 kHz for 31.75 (FrSky D), the MPM v2's at 30 kHz.
Sensitivity untested (the MPM's weakest power still gives -73 dBm; the bad
packets per airtime are about the same for every rate, Wi-Fi at 2440 MHz).

Against the 4-in-1's CC2500 (bench presets `frskyd`, `frskyx`, `ch 0`; the
CC27 raw on the CC2500's sync word: `AAAAD391` for FrSky D's 16 bits,
`D391D391`, length byte, payload and CRC received as data): 100%, same
polarity as the CC2500 and the CC2652. FrSky X at PDIF decimation 1 and 2,
FrSky D at 2 only (at 1: 92 degrees per sample). The 4-in-1 sits about
70 kHz (X) and 90 kHz (D, FSCTRL0 -25) below the CC27's frequency; windows
of about 100 kHz (X) and 70 kHz (D). Trap: a strong sender (-39 dBm) also
shows up mirrored about the CC27's channel centre, inverted bits included
(4-in-1 at -70 kHz: received with the inverted sync word at +30 to +90 kHz),
which looks like a polarity difference.

FrSky D's aliases: at 31 kbps the samples' rate is 248 kHz (4 x rate x
PDIF decimation 2), and a sender 248 kHz off is received as well as one on
the channel, CRC good, the frequency estimate wrapped (2026-11-01: 4-in-1
`frskyd` on channel 0, CC27 trim swept by 20 kHz: 100% from -60 to 0 kHz
around the real centre, again from -280 to -240 and from +200 to +240,
nothing in between). Nothing on the CC27 removes them: BDE2 16 or 31 and
BDE1 decimating receive nothing at any digital gain (BDE2DECRATIO is a
plain ratio whatever the header's log2 names: `LRF_programPQ()` multiplies
by it); the RX at 2 or 4 x the rate (497 kHz) receives 0 to 18% at any
offset, the vote's samples offset searched or not; an IF of -1 MHz changes
nothing. They only matter to FrSky RX's tune search
([mpm-core.md](mpm-core.md#rx-protocols)): FrSky D's telemetry comes on the
channel the TX listens on (a D8R-II+: 27 frames/s, as the MPM v2), and the
next channel's sender is 52 kHz from an alias's centre (windows about
60 kHz wide).

Our OpenOCD branch `cc27xx`
(`~/Src/openocd-cc27xx`, configure `--enable-xds110 --enable-cmsis-dap`) has a
flash driver for it, through its XDS110:

```sh
openocd -f board/ti/lp-em-cc2755p10.cfg -c "program app.elf verify reset exit"
```

On a booted chip the boot ROM's flash functions erase and write single
sectors (928 KB in 22 s with verify): the HSM firmware and the other sectors
stay. `reset halt` stops at the application's first instruction, without a
reset pin (SEC-AP reset, then SACI exit). Sectors the CCFG protects
(`flashProt`; TI's examples protect the CCFG and SCFG) need a chip erase
first, which keeps the HSM firmware; the writes then go through SACI:

```sh
openocd -f board/ti/lp-em-cc2755p10.cfg \
  -c "init; cc27xx mass_erase; flash write_image app.elf; reset run; shutdown"
```

(12 s for main + CCFG + SCFG). A blank chip stays in SACI after a reset and
is written the same way (`catch {reset halt}` or `mass_erase` first). A CCFG
with `flashProt.writeEraseProt.auxSectors` bit 0 set (CCFG +0x2C) leaves the
CCFG rewritable without a chip erase; the SCFG stays protected whatever the
CCFG says.

`tools/flash_dslite.sh` flashes through UniFlash's DSLite instead:

```sh
tools/flash_dslite.sh app.bin@0x0       # or .elf / .hex; erase, write, verify, reset
tools/flash_dslite.sh -v app.bin@0x0    # verify only
```

Only the sectors the files cover are erased; CCFG and SCFG are kept unless
a file has data there. `$XDS_SERIAL` picks the probe when several XDS110 are
connected, `$XDS_DEVICE` the UniFlash device (default `cc2755p10`), `$DSLITE`
the `dslite.sh` (default: the newest UniFlash in `/Applications/ti` or `~/ti`).

TI's OpenOCD fork ([ti-openocd](https://github.com/TexasInstruments/ti-openocd),
branch `ti-release`, needs `set DAP_SWD_ID 0x6ba02477`) chip-erases for every
erase without retaining the HSM firmware ("HSM failed to boot" in UniFlash):
`dslite.sh -s HsmImagePath=<sdk>/bin/hsm/CC2755R10_hsm_fw_v3.5.0.bin -b
ProgramHsmImage` puts it back without touching the rest.

`src/core/bench.c` is the command interpreter, `src/platforms/cc26xx/radio.c`
the RF core driver (bare-metal proprietary mode: `rfc.c`/`rfc.h` and the
proprietary CPE patch are the only TI radio files), `src/core/frame.c` the
framings of the MPM chips.

## Host tools

Everything is in `tools/bench/`. The scripts are `uv` scripts (pyserial is
installed on the fly): run them as `uv run tools/bench/<script>` or directly.

Ports: `$MPM_PORT`, `$CC_PORT` and `$V2_PORT`, otherwise the first STM32duino
USB serial port (VID `1eaf`), the first XDS110 port and the first CH343 port
(MPM v2, `1a86:55d3`). `CC_PORT=v2` runs any script with the MPM v2 as the
CC2652. Each script sets the CC2652 bench back to its own radio (`chip
cc2652`), then `CC_INIT` adds commands on it (e.g. `CC_INIT="pwr 0x12d6"`, -5
dBm into the PA: +20 dBm at 1 m overloads some 4-in-1 receivers;
`CC_INIT="chip sx1281"`: the MPM v2's SX1281 sends and receives instead).

```sh
tools/bench/mpm_bench.py ports
tools/bench/mpm_bench.py shell                       # MPM console
tools/bench/mpm_bench.py -p cc shell                 # CC2652 console
tools/bench/mpm_bench.py run tools/bench/examples/frskyd.txt
tools/bench/mpm_bench.py exec "preset frskyd" "ch 10" "rx 20" --listen 10
tools/bench/link.py --mpm frskyd.txt --cc cc2652_frskyd.txt \
    --payload 110102030405060708090a0b0c0d0e0f1011 --cc2500
```

`mpm_bench.py` can be imported too: `Bench(port).cmd("ch 10")`,
`Bench.packets(seconds)`. `rig.py` holds what the test scripts share: device
setup, setups as `;`-separated commands or example files
(`"hott.txt;ch 10"`), the received packets (`packets()`), the two-way link
test, a suite's run (`run_suite()`) and the suites' tests (`suite_cases()`,
`suite_setups()`: what `compare.py`, `losses.py` and `autotrim.py` run).

| Script | What it does |
|---|---|
| `mpm_start.py` | The 4-in-1 waiting in its USB boot loader after power up ("Maple 003") starts its firmware, nothing written (libusb: DFU download request, USB reset before the data reaches flash) |
| `link.py` | Two-way link test with any pair of setups: numbered packets (the counter is at byte 2), all bytes compared, CRC flags checked |
| `suite_nrf.py` | NRF24L01 at 250 kbps / 1 Mbps / 2 Mbps with 6 addresses (2 Mbps: `nrf2mble` both ways, `nrf2m` CC2652 -> MPM), XN297 standard and enhanced modes |
| `esb.py` | Enhanced ShockBurst: CC2652 PTX with auto-ack and retransmissions, MPM NRF PRX, ack payloads. `--rate 2mble`: 2 Mbps on the Bluetooth LE 2M PHY |
| `suite_cc2500.py` | The CC2500 protocols (18 setups), MPM CC2500 registers against the CC2652 presets. `--phy` derives a CC2652 PHY from any CC2500 register table |
| `suite_a7105.py` | The A7105 protocols (11 setups), with their `A7105_Init()` tables |
| `suite_cyrf.py` | The CYRF6936 DSSS protocols (12 setups), with their own register setups |
| `losses.py` | Where the losses of a CYRF6936, A7105 or plain NRF24L01 suite setup go: missing or bad, wrong bytes and bits, RSSI, receiver counters, and whether they lock to the 102.4 ms Wi-Fi beacon cycle (`--timing`). `--mpm-extra` / `--cc-extra` change one register or setting at a time |
| `sdr.py` | Client for an ESP32-S3 running [esp-sdr](https://github.com/ESPARGOS/esp-sdr): I/Q bursts, FM demodulation, bit clock and slicing. `sdr.py info`, `sdr.py cap <MHz>`, `sdr.py survey`: an unknown transmitter's channels, bit rate, sync word and first bytes |
| `sdr_compare.py` | The same A7105 or NRF24L01 packet from the MPM and from the CC2652, seen by the SDR: bit rate, carrier, deviation in runs and on isolated bits, worst bit, payload; `--bits` per-bit table |
| `proto_capture.py` | The MPM runs one of its protocols (`proto`), the CC2652 receives on one channel: E01X, E129, C186, E010R5, FrSky L |
| `frskyl_reverse.py` | FrSky L CC2652 -> CC2500: 60 byte chunks of the chip stream, compared bit by bit |
| `freqscan.py` | The CC2652 receives or sends at several frequency offsets: where a chip really is, the offset between the two boards |
| `timing.py` | Bench `timing` per PHY (NRF, FrSky D and X, AFHDS2A, DSM): TX start, hop, turnarounds, RSSI, as a table ([radio-interface.md](radio-interface.md)) |
| `autotrim.py` | `autotrim`: the CC2652 learns the trim from the MPM's packets, then sends to it; `--sweep`: the frequency estimate (`foff`) per trim |
| `radio_sim.py` | The radio side of the MPM serial link for the firmware without `BENCH` (MPM core): frames every 7 ms at 100000 8E2, status, protocol list scan, bind, test protocol to the 4-in-1, `drive` a real receiver (bind, CH1-4 sweep, FrSky link telemetry), `sport` S.Port requests to a FrSky X receiver as a Lua script (UNI setup fields, `--push`); `--eeprom` / `--save-eeprom FILE`: the module's EEPROM loaded before, saved after ([mpm-core.md](mpm-core.md#files-over-usb)) |
| `proto_compare.py` | A protocol ported to the MPM core (LaunchPad) against the 4-in-1 running it, same module id and channels: the MPM v2 receives both, bind phase and data, payloads and intervals compared; `--low-power` / `--range`: the LaunchPad's power flag ([mpm-core.md](mpm-core.md#mpm-protocol-files)) |
| `rx_test.py` | The MPM core's RX protocols (FrSkyRX, FS2A_RX, BayanRX, DSM_RX) on the port receiving the 4-in-1: bind, then channels and frame rate each second against the 4-in-1's; `--rx-sub 1` a CloneTX capture ([mpm-core.md](mpm-core.md#rx-protocols)) |
| `power_sweep.py` | TX power curves: the MPM v2's or the LaunchPad's RF core words, the 4-in-1 chips' power registers, the RSSI at another board, steps below full power; `recip`: the MPM v2's output from the path both ways ([interop-results.md](interop-results.md#tx-power)) |
| `xk_sniff.py` | An XK transmitter (WLtoys cars, XK planes) heard by a real XN297 receiver (the 4-in-1's nRF24L01, or a CC2652 bench): its bind packets give the GID, then the data address and channel derived from it; one line per payload change, the values each byte took ([mpm-core.md](mpm-core.md#real-receivers)) |
| `car_drive.py` | A toy car (XK Cars, V2x2) driven by the MPM core through a known sequence: bind, steering, throttle, brake / reverse, signal loss, each step printed with its time ([mpm-core.md](mpm-core.md#real-receivers)) |
| `baysim.py` | The module bay simulator (`baysim/`, C firmware for an RP2040-Zero: GP12 to the bay's PPM pin, GP13 from S.Port, GP11 the module supply): `flash` (EdgeTX's update sequence through `stk500_flash.py`, the first frames after power on), `window drive\|float\|frames\|ppm` (the boot loader's window: radio present, no radio, serial frames, PPM), `reboot` (frames all along, the module power cycled: the time to its first telemetry), `ppm` (PPM on the PPM pin: channels, frame, pulse, polarity), `pwr`, `level`; `radio_sim.py -p <link port>` drives the MPM core over the bay ([update.md](update.md#tested-over-the-module-bay)) |
| `servo_mon.py` | A real receiver's servo output, measured by an RP2040-Zero (`rp2040/main.py`, MicroPython): its supply switched on GP11, pulses, width and period on GP12 each second, gaps; a second input on GP9, another servo output or SBUS (frames, CH1-4, frame lost and failsafe flags) ([mpm-core.md](mpm-core.md#real-receivers)) |
| `frsky_receiver.py` | A bound FrSky receiver on `servo_mon.py` (SBUS on GP13, tied to GP12, or a servo output), driven by the port or the 4-in-1 with the port's id: `rate` (outputs each second, SBUS flags, telemetry), `loss` (signal off and on: the frame lost and failsafe flags, the time to the new position), `cold` (power up: the time to the first output) ([mpm-core.md](mpm-core.md#real-receivers)) |
| `dsm_receiver.py` | A real DSM receiver on `servo_mon.py`, power cycled into bind (bind plug in), bound and driven by the port or the 4-in-1: `bind` (any sub protocol, Auto as EdgeTX) with the bind responses, pulse rate and period; `loss`: signal off and on, the output meanwhile and the time to reconnect ([mpm-core.md](mpm-core.md#real-receivers)) |
| `raw_capture.py` | The CC2652 captures raw bits after a sync word of its choice: how the on-air formats were worked out |

### ESP32-S3 as a third receiver

An ESP32-S3 flashed with esp-sdr (the browser installer of the project) is an
8-bit receive-only SDR on USB: bursts of 16380 samples, about 1 ms at
16 MS/s. It shows what a chip really sends, independently of the two chips
under test. Found out while setting it up:

- After flashing, unplug and replug: it then enumerates as "USB JTAG/serial
  debug unit" (303a:1001). `$SDR_PORT` overrides the port.
- The spectrum is inverted (`sdr.demod()` conjugates). `FREQ` takes whole
  MHz: tune 1 MHz off the signal, away from the DC spike.
- The hardware AGC only catches the start of a packet: manual gain about 40
  at 1 m (`Sdr.autogain()` picks the highest gain without clipping).
- The ESP32 crystal is good to 10 to 20 ppm: compare two senders, don't take
  absolute frequencies.
- 16 MS/s keeps every fifth sample with no digital filter, and the default
  analog filter is about 69 MHz wide: a signal 12 MHz off shows 16 MHz away
  from where it is. `BANDWIDTH 13` (what `survey` sets) removes these aliases.

`sdr.py survey` identifies an unknown transmitter: windows every 5 MHz from
2401 to 2481 MHz, each keeping carriers 0.8 to 6 MHz from its centre. Per
packet: carrier, bit rate (lowest of 31 kbps to 2 Mbps whose transitions line
up), deviation, then the sliced bits against known sync words (A7105 IDs,
CC2500 d391, XN297 preamble, V2x2 nRF24 address, BLE advertising). XN297 addresses are shown
descrambled, as given to `XN297_SetTXAddr()`. Checked on the 4-in-1 running
FlySky (sync 5475c52a, then 55 + TX id, 5 MHz hop grid), AFHDS2A (packets
longer than a capture), Bayang (address = module id) and FrSky D (31 kbps; a
few captures read as 2 or 3 x the rate, flagged in the summary). The
carrier is taken midway between the two tones, not at the median: a bind
packet of mostly zeros sits on one tone. Found this way: a WLtoys 1/28 car
transmitter is V2x2 (1 Mbps, 285 kHz deviation, about 50 us of carrier
before the preamble). Its bind packets (ID 7f 08 c8, flags c0, CRC checked)
hop on the 16 channels of the ID 0 table (row 0, increment 0), where the MPM
binds on its own ID's table.

`sdr.py v2x2` stays on one window and prints each V2x2 packet whole (16
bytes, the nRF24 CRC checked, fields as `V2X2_send_packet()` fills them),
then the values each byte took: about one packet a second. The car's
transmitter sends throttle in byte 2 (the MPM's elevator, 7f forward, ff
reverse), steering in byte 1 (rudder, ff left, 7f right), the steering and
throttle trims in bytes 4 and 5 (40 at centre), 00 in bytes 0, 6 and the
flags. The 4-in-1 (V2x2, RX number 0: bind channels 3 MHz from the car
transmitter's) binds the car without its bind button and drives it; the car
keeps the bind across a power cycle and stops at once on signal loss.

```sh
tools/bench/sdr.py info
tools/bench/sdr.py survey                    whole band, 20 captures per window
tools/bench/sdr.py survey 2411 --n 200 -v    one window, each packet
tools/bench/sdr.py v2x2 2440 --secs 60       V2x2 packets, whole, then the bytes that moved
CC_PORT=v2 tools/bench/sdr_compare.py flysky
CC_PORT=v2 tools/bench/sdr_compare.py joysway --bits 48 64
CC_PORT=v2 tools/bench/sdr_compare.py nrf2m --ch 40
tools/bench/sdr_compare.py nrf2m --cc-extra "phy 2000000 500000 100"
```

`sdr_compare.py` measures the carrier and the deviations from the eye center
(midway between the mean levels of ones and zeros): `sdr.demod()`'s carrier,
the burst's median frequency, falls towards the more frequent level, 140 kHz
off on CC2652 2 Mbps packets.

Each suite takes protocol names (default: all), `--count`, `--period`, `--dir
fwd|rev` (fwd: MPM -> CC2652) and `-v` (print the setup commands), and exits
with 1 if a packet is lost.

```sh
tools/bench/suite_cc2500.py hott hitec --count 500
tools/bench/link.py --mpm "preset hubsan" --cc "trim 90;preset hubsan" --len 16 --ch 170
tools/bench/proto_capture.py e012
```

Unit tests of `src/core/frame.c` on the host: each framing against the MPM code
(XN297, HS6200, RF2500, FrSky L encoders, copied or ported from the MPM) and
against raw captures of the MPM chips (ESB, HoTT, A7105, DSM, Devo, Kyosho3).

```sh
make -C tests/frame
```

## Line protocol

One ASCII command per line (`\n` or `\r\n`). Numbers are decimal or `0x` hex.
`<hex>` is a byte string: `0a1b2c`, `0a 1b 2c` or `0x0a 0x1b 0x2c`.

Each command gets exactly one answer line, `@ok [data]` or `@err <message>`.
Asynchronous events are lines starting with `@rx`, `@ack`, `@ev` or `@trim`. All other
lines are debug output and are ignored by the host.

Received packets: `@rx <time_us> <channel> <hex> [crc=ok|bad] [...]`.

### MPM commands

| Command | Description |
|---|---|
| `info` | Reset all chips, report their presence: `@ok bench=1 chip=.. job=.. ch=.. cc2500=1 nrf=1 a7105=1 cyrf=1` |
| `chip <cc2500\|nrf\|a7105\|cyrf>` | Select a chip (RF switch + reset) |
| `preset <name> [arg]` | Load a known-good configuration taken from an existing protocol. CC2500: `frskyd`, `frskyx`, `frskyxeu`, `250k` (arg = frequency offset). A7105: `afhds2a`, `flysky`, `hubsan`, `bugs`, `height`, `height8`, `joysway`, `kyosho`, `kyoshohype`, `pelikan`, `wfly2` (the protocol's `A7105_Init()` table, includes VCO/IF calibration). CYRF6936: `dsm` (DSM data packets: 8DR, SOP, length, CRC, PN row 0 / SOP column 0 codes, CRC seed 0), `dsmbind` (bind packets: SDR, no SOP, length or CRC). NRF: `nrf` |
| `xn <crc> <scramble> <250k> [force_nrf] [enhanced]` | Select the XN297 emulation. 1 Mbps runs on the NRF, 250 kbps on the CC2500 unless `force_nrf`. `enhanced`: XN297 enhanced mode (packet control field) |
| `xaddr <hex>` | XN297 TX and RX address, 3 to 5 bytes |
| `ackpl [hex]` | NRF ack payloads (pipe 0) for an RX job with EN_AA and EN_ACK_PAY, the TX FIFO is kept full. The byte at the `seq` index counts. None to clear |
| `w <reg> <hex>` | Write register(s), burst when several bytes (CC2500 PATABLE, NRF addresses, A7105 ID...). Values are hex bytes: `w 0x06 60` writes 0x60. CYRF6936: no address increment, see below |
| `r <reg> [n]` | Read n register(s): `@ok <hex>`. CC2500 status registers (0x30+) are handled |
| `s <cmd>` | Strobe / command byte |
| `ch <n>` | RF channel: CC2500 `CHANNR`, NRF `RF_CH`, A7105 `PLL_I`, CYRF6936 `CYRF_ConfigRFChannel()`, XN297 NRF numbering |
| `tx <hex>` | Send one packet. CC2500: the hex is the FIFO content, so include the length byte in variable length mode |
| `txp <period_us> <count> <hex>` | Send periodically, `count=0` = forever. `@ev txdone <n>` at the end |
| `seq <idx>` | `txp` puts an 8 bit counter at byte `idx` of the packet, `255` = off |
| `rx [len]` | Receive continuously, see below |
| `stop` | Stop the current job: `@ok stop tx=<n> rx=<n> err=<n>` |
| `chan <i> <value>` | Channel value 0..2047 (1024 = center) used by `proto` (CH5 to CH12 switches updated too) |
| `id [value]` | Module id used by `proto` (`MProtocol_id_master`), as the CC2652's to compare a port: `@ok id=<hex>` |
| `cyrfid [hex]` | CYRF6936 MFG ID used by DSM (6 bytes, RAM only: the EEPROM copy and its binds stay), none: the chip's again. `@ok cyrfid=<hex>` |
| `proto <num> [sub] [rxnum] [option] [bind]` | Run a built-in protocol with the normal scheduler, `num` as in `Multiprotocol.h` |
| `user [hex]` | Run `Bench_user_init()` / `Bench_user_callback()` from `_Bench_User.ino` |

| Chip | `rx` length | Reported data |
|---|---|---|
| CC2500 | Bytes to read from the FIFO, including the appended status. `0` = read the FIFO when the radio goes back to IDLE, which needs `MCSM1.RXOFF_MODE`=IDLE | Raw FIFO: length byte if variable length, payload, RSSI and LQI\|CRC_OK if `PKTCTRL1.APPEND_STATUS` |
| NRF | Payload size, `0` = `RX_PW_P0` or dynamic payload | Payload. With CRC enabled, bad packets are dropped by the chip. `rpd` = received power > -64 dBm |
| A7105 | Payload size (required: the FIFO length register is write-only) | Payload, `crc` from the `MODE` register |
| XN297 | Payload size (required) | Decoded payload, `crc` checked in software |
| CYRF6936 | Ignored: `RX_COUNT` bytes are read | Payload (no length byte), `crc=ok` when `RX_IRQ_STATUS` has RXC without RXE (bad CRC or symbol errors) |

`_Bench_User.ino` runs in the same µs scheduler as the protocols, for tests
the host can't time precisely over USB (hopping, answering within a fixed
delay). The example is a ping: it sends a packet, waits for the echo and
reports the round-trip time.

```
preset frskyd
user 05 64 01 14 11 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10
     |  |  |  |  ^ packet = FIFO content (CC2500: length byte 0x11 + 17 bytes)
     |  |  |  ^ rx_len = 20 bytes for the answer, as for the "rx" command
     |  |  ^ index of the sequence number in the packet
     |  ^ period_ms = 100
     ^ timeout_ms = 5
-> @ev pong <seq> <rtt_us> <hex> | @ev timeout <seq>
```

### CC2652 commands

The radio is a raw 2-GFSK / 2-FSK packet engine (preamble, sync word, fixed
length data); the framing of each chip is done in software (`frame.c`). The
`ch` numbers of a preset follow the chip it emulates.

| Command | Description |
|---|---|
| `chip <cc2652\|sx1281> [dbm]` | Radio for the PHY (applied at once and kept): the RF core, or the MPM v2's SX1281 (GFSK 125 kbps to 2 Mbps, rates within 1%, nearest modulation index, TX power in dBm into the PA: default 0, max 2). `info` shows what the SX1281 got, `stop` its counters (`sx_*`). `ack` works with both |
| `preset <nrf1m\|nrf2m\|nrf2mble\|nrf250k\|xn250k>` | NRF24L01 compatible PHY. `xn250k`: the MPM CC2500 XN297L emulation (127 kHz deviation). `nrf2mble`: 2 Mbps on the RF core's Bluetooth LE 2M PHY (500 kHz deviation in TX), which receives 2 Mbps, unlike `nrf2m`, and sends it without `nrf2m`'s 2 to 3% losses at ch 30 to 41; 32 bit sync word (not tried above 255 byte packets) |
| `preset <name>` | CC2500 PHYs of the MPM protocols, with their channel plan and `cc2500` framing: same `ch` numbers as `CHANNR`. `frskyd`, `frskyv`, `frskyx`, `frskyx2`, `frskyxeu`, `frskyx2eu`, `hott`, `hitec`, `redpine`, `redpineslow`, `skyartec`, `corona`, `coronav2`, `fdv3`, `ares`, `radiolink`, `ikea`, `sfhss`, `tfhss` (Futaba T-FHSS: 384 kbps MSK, 14 bytes, no CC2500 CRC; `ch` = `CHANNR` = 5 (n - 1) for channel n = 1..31). Sync words from the ID (Hitec, Skyartec data packets, RadioLink variants): `cc2500` after the preset |
| `preset <flysky\|afhds2a\|hubsan\|bugs\|height\|pelikan\|kyosho\|kyoshohype\|wfly2\|joysway>` | A7105 PHYs with their `a7105` framing and channel plan (2400 MHz + `ch` x 500 kHz, `ch` = `PLL_I`) |
| `preset <dsm\|dsmbind\|devo>` | CYRF6936 PHY (1 Mchip/s GFSK, 250 kHz deviation) with the `cyrf` framing and the codes of the MPM presets (`devo`: `examples/devo.txt`), channel plan 2401 MHz + `ch` x 1 MHz |
| `preset gfsk1m` | CYRF6936 GFSK mode PHY (E01X, E129, E010R5): 1 Mbps GFSK, 250 kHz deviation, 2400 MHz + `ch` x 1 MHz. Then `hs6200` or `rf2500` |
| `preset frskyl` | FrSky L: 125 kbaud 2-FSK, 47.5 kHz deviation, FrSky channel plan, `frskyl` framing (the 256 byte chip stream, 11 chips per bit), `prefix 4e` for the RX sync word. `rx 17`; the packet CRC is not checked |
| `phy <rate> <dev_hz> <rx_bw> [preamble_bytes] [if_freq] [fsk]` | Custom PHY. `rx_bw` is the RF core code (85 = 175 kHz, 87 = 265 kHz, 89 = 350 kHz, 94 = 882 kHz, 97 = 1399 kHz, 100 = 2458 kHz). `fsk=1`: 2-FSK instead of 2-GFSK |
| `pwr <word>` | RF core TX power word, applied at once (`CMD_SET_TX_POWER`, the next packet) and kept for the next `preset`/`phy`; the SX1281 at the next `preset`/`phy` (default `RF_TX_POWER_DEFAULT` in `board.h`: `0x7217` = +5 dBm on the LaunchPad, `0x2853` = 0 dBm on MPM v2, in front of a 22 dB PA whose input is +5 dBm max) |
| `ovr [word...]` | Up to 8 raw RF core override words (`HW_REG_OVERRIDE()` etc., see [rf-core-notes.md](rf-core-notes.md)) appended to the setup of both PHYs, applied at once and kept; `ovr` alone clears them |
| `syncthr <n>` | Sync word correlation threshold (RF core register 0x5114), `0` = auto: TI default 0x1F, 0x32 when the RX sync word is weak. Applies at once |
| `sync <hex>` | Raw framing, 1 to 4 byte sync word |
| `nrf <addr> [crc_bytes] [sync_bytes]` | NRF24L01 framing (EN_AA=0, static payload): address in `TX_ADDR` order, CRC computed in software |
| `esb <0\|1> [dpl] [noack]` | After `nrf`: Enhanced ShockBurst, 9 bit packet control field (what the nRF24L01+ sends with EN_AA or DYNPD). `dpl` (default 1): dynamic payload length, `rx <len>` is then the maximum. `@rx` lines get `pid=<n>` |
| `ack <0\|1> [retries] [window_us] [ack_max_len]` | ESB transmitter with auto-ack, as the nRF24L01+ with EN_AA: after each packet, RX for `window_us` (default 500), up to `retries` (default 3) retransmissions. `tx` reports `acked`, `tries` and the ack; `txp` prints `@ack <us> <ch> <hex> tries=<n> rssi=<dBm>` for ack payloads and `stop` adds `acked` / `retries` |
| `xn <crc> <scramble> <250k> [force_nrf] [enhanced]` | XN297 framing, same arguments as on the MPM (`force_nrf` is ignored). Loads `preset nrf1m` or `xn250k` |
| `xaddr <hex>` | XN297 address, 3 to 5 bytes, as given to `XN297_SetTXAddr()` |
| `prefix [hex]` | Known first payload bytes, up to 8 (none to clear): more known bytes to choose the RX sync word from. Not with a packet control field (ESB, XN297 enhanced mode) |
| `cc2500 <sync> [crc] [var] [white] [fec] [msk]` | CC2500 framing: `SYNC1 SYNC0`, 4 bytes (`d391d391`) for `SYNC_MODE` 3 (sync word sent twice). `crc` (default 1): CC2500 CRC-16 (`CRC_EN`), `2`: CRC with `CC2400_EN` (SFHSS). `var` (default 1): variable length, the payload starts with the length byte as in the CC2500 FIFO. `white`: PN9 whitening. `fec`: CC2500 FEC, Viterbi decoder on RX. `msk`: MSK, all bits inverted (use a 2-FSK PHY with a deviation of rate / 4). The RX sync word includes the last 2 preamble bytes |
| `a7105 <id> [crc] [fec]` | A7105 framing: ID (2 or 4 bytes, as given to `A7105_WriteID()`, the sync word), `crc` (default 1): CRC-16 CCITT from 0x1D0F over the payload. `fec` (default 0): Hamming (7,4) per nibble, nearest code word on RX |
| `cyrf <sop\|0> <data_code> [seed] [crc] [len] [sdr] [data_chips] [invert]` | CYRF6936 framing: SOP code (8 bytes as given to `CYRF_ConfigSOPCode()`, 4 bytes for a 32 chip SOP, `0` = no SOP), data code (16 bytes as given to `CYRF_ConfigDataCode()`, SDR: 8), CRC seed (`CRC_SEED_MSB << 8 \| LSB`, default 0), `crc` (default 1), `len` (default 1): length field, `sdr` (default 0): 1 bit per code instead of 8DR, `data_chips`: 32 or 64 (default), `invert`: all chips inverted (`TX_OVERRIDE` TX_INV, Kyosho3). After a `preset dsm`/`dsmbind`/`devo` (PHY) |
| `hs6200 <addr> [crc] [scramble]` | HS6200 framing (E01X): address as given to `HS6200_SetTXAddr()` (3 to 5 bytes), CRC and scrambling (default 1), the no ack bit set as the MPM does. `rx <len>`: maximum length. `@rx` lines get `pid=<n>` |
| `rf2500 <addr> [scramble] [crc]` | RF2500 framing (E129 / C186: `scramble` 1, 16 or 19 bytes; E010R5: 0): address as given to `RF2500_SetTXAddr()`, 4 bytes, CRC (default 1) |
| `ch <n>` / `freq <khz>` | Channel of the channel plan (default 2400 + n MHz) / any frequency |
| `chplan <base_hz> <spacing_hz>` | Channel plan, e.g. CC2500 `FREQ` and `CHANSPC`. Presets set it |
| `trim <khz>` | Frequency correction added to `ch` and `freq`, as the MPM CC2500 `option` (`FSCTRL0`). It persists across presets: always set it |
| `autotrim [off\|<offset_khz>]` | During `rx`, `trim` follows the senders: every 8 packets with a good CRC, moved by the mean of their `foff` + offset when 5 kHz or more, so that the CC2652 sits at the sender's frequency + offset (A7105: 35). Prints `@trim <khz>`. Until `autotrim off` |
| `peek <addr> [n]` / `poke <addr> <val>` | Read up to 16 words / write one (RF core registers: see [rf-core-notes.md](rf-core-notes.md) for what hangs the bus or stops RX) |
| `sched <period_us> [n]` | `timer_set_alarm()` chained on absolute deadlines (previous + period), `n` alarms (default 100): `@timing sched_late`, the callback start after its deadline. An `rx` job goes on meanwhile |
| `fasst [secs] [invert] [chip_rate] [dev_hz] [fs_every] [sweep] [step] [id] [7ch]` | Futaba FASST 10ch TX for `secs` (default 10, blocking; [mpm-core.md](mpm-core.md#futaba-fasst)): channels 200 + 170 i, CH1 swept (16 every `sweep` frames, default 2, 0 = fixed), the F/S cycle (positions 1600 - 100 i) every `fs_every` frames (default 0: none). `step`: the hop step (1 to 36, default 27, the TM-10's; 41 to 62: Area France, from hop 1). `id`: the plain ID, packet bytes 0 to 3 (default `0x12689fe0`, the TM-10's). `7ch` 1: FASST 7CH (`fasst7_packet()`), one 17-byte packet per 8 ms, channels 1320 (1520 µs), CH1 swept 820 to 1820, CH3 1720; with `fs_every` (2 or more), CH3's F/S (1020, 1220 µs) in every `fs_every`-th packet. Defaults 1.5 Mchip/s, 500 kHz, `pwr` as set; the PHY in use is set up again after it |
| `fasstest [secs] [row] [sweep] [id0..3] [id4..5] [link_secs] [pages] [rx]` | Futaba FASSTest 14CH TX for `secs` (default 10, blocking): one packet per 15 ms hop on hop row `row` (0 to 22 set A, General; 23 to 45 set B, France; default 0), the T14SG's packet layout (mode 0x13, ch1..8 and ch9..16 every other packet, no telemetry request): channels 352 + 84 i (S.BUS values), ch1 swept (16 every `sweep` frames, default 2, 0 = fixed). `id0..3` / `id4..5`: the TX ID (default `0x088aaffb` `0`, the T14SG's: a receiver linked to it follows). `id0..3` at most 999999999 (the receiver saves it as 9 decimal digits). `link_secs` (default 0): link packets first, up to that long, on the T14SG's schedule (the receiver's link channels 2, 8 ... 44, each every 30 hops, 4 hops apart), each followed by a listen on its channel: a receiver powered up meanwhile takes the ID and answers (`@fasst` line, then `@link rx=<serial>`), and the data start 2 hops after the link packet. Every packet heard in the listens is printed. `pages` (bit p = page p, 0 to 3; default 0 = no telemetry): a telemetry request every 10 hops (the T14SG at its 0.3 s rate), the pages in turn; the next hop is left out and its channel listened to for the reply, unwhitened with `rx` (the receiver's serial, default `0x08a18240`, the lent R7008SB's; the link reply's if one came): `@tele <time_us> <channel> rssi= page=<p> <hex>`, the 16 bytes = the 2 data bytes of S.BUS2 slots 8p to 8p + 7 (slot 0: the receiver's voltage, 0.1 V, first byte bits 6..7 = link quality 0 to 3; `c4 00` = EXT voltage). Other packets heard there: `@fasst` lines. Ends with `@ok fasstest links= heard= replies= frames= requests= tele= tele_bad= late= errors=` |
| `sfhss [secs] [mode] [id] [code] [b34] [fs_ms] [w1..w8]` | Futaba S-FHSS TX for `secs` (default 30, blocking; a newline ends it), `preset sfhss` set up for it: per 6.8 ms hop on channel n (`ch` 6n + 16) two 13-byte packets 1.6 ms apart, the next n = n + `code` + 2 mod 31 skipping 30 (`code` default 13). `mode` 0 as the T14SG (ch1..4 command `00`, then ch5..8 `09`), 1 as the MPM's `Futaba_cc2500.ino` (the halves' order alternating per hop, command bit 1). The receiver's link ID is `id` (bytes 1..2, default `0xfbaf`) and `b34` (bytes 3..4, default `0x8a08`: the T14SG's, its FASSTest TX ID low byte first). Values in µs, 1520 centre: CH1 swept 1100 to 1940, CH2 1100, CH3 1940, the others 1520. With `fs_ms`: one hop of F/S packets (command bit 2) every `fs_ms` (100 or more), carrying the raw 12-bit words `w1..w8` (default the T14SG's: `0x800` + the offset from 1520 in 1.25 µs steps, signed 10 bits). Ends with `@ok sfhss hops= late= errors= fs_hops=` |
| `fasstrx [secs] [k] [bytes] [rx_bw] [syncthr] [follow] [fasstest]` | Futaba FASST receiver (sniffer) on hop `k` (0 to 35, default 0) for `secs` (default 10, 0 = until stopped; blocking: a newline ends it, other bytes are dropped meanwhile): each packet despread on the MCU, `@fasst <time_us> <k> rssi= [foff=] shift=<n> <hex> q=<digits>`: the hop it was heard on, the first `bytes` packet bytes after the sync word (default 23, bits LSB first as on air, still whitened) and per byte its worst Barker correlation (`b` = all 11 chips right, `1` = noise: past the packet's end). `shift`: bits from the RF core's sync match to the real sync word (it also matches at the start of the preamble), `sync=none` if not found. With `follow` (default 1), a 10ch packet with a good CRC locks on the hops: its hop index and step give the next hop, 7 ms later (Area France too: step field 40 + s, hops 1 to 22, k + s mod 23 skipping 0); a hop without a good packet keeps the timing, 36 in a row go back to `k`. Ends with `@end fasstrx packets= crc_ok= missed= locks= restarts= buf_full=` (`missed`: hops followed without a good packet). RX bandwidth code default 100 (2458 kHz). With `fasstest` 1 (14CH, 11 chips, 20 bytes) or 2 (12CH: Barker-7 `1110010` chips, 12 bytes, 6.3 ms hops): FASSTest, `k` = its channel (0 to 75, (2347 + k) x 1.024 MHz; FASST hop k = channel 2 + 2k); no CRC check (the whitening needs the TX ID): a TX packet whose first 20 (12) bytes have every bit at most one chip off (`q` 9 or more, 5 with 7 chips) and a known hop code locks, the next channel from its row, 15 ms (12CH 6.3 ms) later; the RX stays on the channel until 1 ms before the next packet, so the receiver's replies (type 0x10, 12CH 0x13, in the slot of a hop the TX leaves out) are printed too. `@end` then has `clean=` and `replies=` instead of `crc_ok=` |
| `timing [n] [len]` | RF core latencies on the current PHY, framing and channel, `n` runs each (default 20), `len` byte payloads (default 16): `@timing <what> n= min= avg= max=` in us, `@edges <what>` the PA / LNA enable edges of the first run. The RF core's PA enable (`RFC_GPO1`) is read back on the MPM v2's PA pin, on the LaunchPad it and the LNA enable (`RFC_GPO0`) go to the LEDs during the test. See `timing.py` |
| `tx`, `txp`, `seq`, `rx <len> [fixed]`, `stop`, `info` | As on the MPM, but the `@rx` time is when the RF core found the sync word. With a length field (`esb` with `dpl`, XN297 enhanced, HS6200, `cc2500` `var`), `rx` receives each packet up to its length (`radio_rx_start_var()`, the reply shows `var=<header bytes>`), `fixed`: the maximum length instead. `@rx` lines have `rssi=<dBm>` and `foff=<kHz>`, the modem's estimate of the sender's frequency above the CC2652 (+-20%, not for acks). The `rx` reply shows the RX sync word and threshold. `stop` adds the RF core counters after `rx` |

The XN297 examples (`xn297_1m.txt`, `xn297_250k_cc2500.txt`) run unchanged on
both devices. The `cc2652_*.txt` examples are the CC2652 counterparts of the
MPM ones.

## Gotchas

General:

- `seq` and `trim` persist on both devices until changed: a `txp` after an
  earlier test may overwrite a payload byte with its counter. The rig sets
  both before each test.
- The MPM bench never runs a protocol's init: `Bench_loop()` sets what the
  chip helpers expect (e.g. `crc16_polynomial` for the XN297 emulation).

MPM CC2500:

- `rx <len>` with `RXOFF_MODE`=IDLE reads the packet before the radio is back
  in IDLE and never restarts RX: use `rx 0`, or `RXOFF_MODE` RX (`w 0x17 0x0C`)
  to chain packets.
- With `RXOFF_MODE`=RX (the FrSky presets) and a fixed `rx <len>`, a sync on
  noise (15/16 sync bits) puts a garbage packet in the FIFO and the next reads
  are shifted: `w 0x17 0x00` then `rx 0`.
- The protocols without autocalibration (HoTT, Hitec, SFHSS) need `s 0x33`
  (SCAL) after each channel change.

MPM A7105:

- The FlySky table receives nothing (FlySky only transmits): receive FlySky
  packets with the AFHDS2A table, FEC off and a 21 byte FIFO (`flysky_rx.txt`).
  Most other protocol tables don't receive either: use the AFHDS2A table with
  their `DATA_RATE` (0x0E), `CODE_I` (0x1F), FIFO length and ID.
- `rx <len>` doesn't set the FIFO length (`FIFO_I`, 0x03): the preset does
  (AFHDS2A 38 bytes, FlySky 21, Hubsan 16), `w 0x03 <len - 1>` for others.
  With a longer FIFO the payload reads right but the CRC is checked on the
  wrong bytes.
- Background 2.4 GHz noise sometimes passes the ID sync (about 1/s with
  `preset afhds2a`): `crc=bad` packets with random data. Filter on `crc`.
- At 2410 MHz (`ch 20`, Wi-Fi channel 1) the A7105 lost a packet about every
  100 ms (Wi-Fi beacons). The suites use `ch 170` (2485 MHz).
- This A7105 is about 90 kHz above the CC2652: `trim 90` on the CC2652.

MPM CYRF6936:

- Multi-byte register writes don't increment the address: one `w` per
  register (CRC seed: `w 0x15 <lsb>`, `w 0x16 <msb>`). The code buffers are
  written whole: `w 0x22` (SOP) 8 bytes, `w 0x23` (data) 16 bytes, even in
  SDR mode: after an 8 byte `w 0x23` the CYRF6936 neither sends nor receives
  with the new code.
- `RX_IRQ_STATUS` (0x07) has `SOPDET` (bit 6), `RXC` (1), `RXE` (0); `RSSI`
  (0x13) bit 7 is set once a SOP was detected.
- Devo, WFLY and Traxxas aren't in the bench build: `devo.txt` writes the
  Devo registers. The CYRF6936 receives the CC2652 even with a wrong SOP
  separator: only the CC2652 side shows such errors.
- TX-only setups need help to receive: `CLK_EN` 0x02 (`w 0x28 0x02`), RX CRC
  check off (`w 0x1E 0x14`), MLINK's `RX_CFG` VLD_EN off (`w 0x06 0x92`).
  WFLY loses its first `rx` after the setup.
- The CYRF6936 sends at 2400 + n MHz, 2401 + n with `TX_OFFSET` 0x0555, and
  receives at about 2401.2 + n or 2402 + n depending on `RX_CFG`: some tests
  move the CC2652 channel plan (`chplan`).

CC2652:

- The RX sync word is chosen among the known bytes (preamble, address, header,
  `prefix`): give a `prefix` when the first payload bytes are known, it can
  only help.
- FrSky L: the CC2652 also receives the channel 48 MHz away (its crystal
  frequency), about 80 dB weaker.
