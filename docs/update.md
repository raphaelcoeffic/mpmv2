# Updating the MPM v2 firmware from the radio

The update the STM32 MULTI-Module has: the user picks a `.bin` on the SD
card ("Flash External Multi" or "Flash Internal Multi" in the SD manager)
and EdgeTX writes it through the module bay, no USB and no tools.

Status: the boot loader (`src/bootloader/`, `-DBOOTLOADER=ON`) works over
USB with a host tool that does what EdgeTX does; the EdgeTX side is on
branch `mpm-v2-flash` (not pushed). Not yet tried from a radio: the module
bay lines (inverted, S.Port polarity through U9) are untested, see
[To test with a radio](#to-test-with-a-radio).

## What EdgeTX does today

`radio/src/io/multi_firmware_update.cpp` (EdgeTX, also used for ELRS
modules with `MULTI_TYPE_ELRS`):

1. **File check.** The last 24 bytes of the `.bin` are the MULTI signature,
   `multi-x<8 hex digits>-<8 digit version>` (`Binary_Signature.ino` in the
   MULTI sources), e.g. `multi-x1234abcd-01020199`. Flags of the hex number:
   bits 0-1 board type (0 AVR, 1 STM, 2 Orange, 3 unused), 0x80 boot loader
   support, 0x100 boot loader check, 0x200 telemetry inversion, 0x400 /
   0x800 telemetry type. For an external module EdgeTX wants (inversion or
   STM) and boot loader support and check and MULTI telemetry (0x800),
   otherwise "needs file ... external multi". Older v1 signatures
   (`multi-stm-bcsid-...`) are read too.
2. **Reset.** `pulsesStop()`, module power off, 3 s, power on, 500 ms. All
   STM32 radios switch the internal and external module power
   (`_internal_module_set_pwr()`, `_external_module_set_pwr()` in
   `boards/generic_stm32/module_ports.cpp`), so the module always starts
   from power up.
3. **Link.** 57600 8N1. The radio sends on the PPM pin (bay pin 1), inverted
   (soft serial), and receives on S.Port, inverted. Internal modules use the
   duplex internal USART instead.
4. **STK500v1 subset** (the replies are `STK_INSYNC` 0x14 ... `STK_OK`
   0x10, each command ends with `CRC_EOP` 0x20):

   | Command | Sent | Expected reply |
   |---|---|---|
   | `STK_GET_SYNC` 0x30 | every 100 ms for 5 s | `14 10` |
   | `STK_READ_SIGN` 0x75 | once | `14 s0 s1 s2 10` |
   | `STK_LOAD_ADDRESS` 0x55 | `lo hi`: word address | `14 10` |
   | `STK_PROG_PAGE` 0x64 | `size_hi size_lo 00 <size bytes>` | `14`, then `10` within 4 x 100 ms (zeros in between are skipped) |
   | `STK_LEAVE_PROGMODE` 0x51 | at the end or on error | `14` (the rest is not read) |

   The signature must start with 0x1E. `1E 55 AA` selects 256 byte pages
   written from word address 0x1000 (byte 0x2000), the STM32 layout with its
   8 KB boot loader; anything else, 128 byte pages from 0. The address goes
   up by 128 words per 256 byte page and only its low 16 bits are sent. The
   last page is padded with zeros (not 0xFF). There is no read back and no
   checksum beyond the UART.

On the STM32 MULTI-Module, the boot check lives in the application
(`CHECK_FOR_BOOTLOADER`, `pollBoot()` in `Multiprotocol.ino`): after power
up the serial input runs at 57600; `0x30 0x20` followed by 5 ms of silence
resets into StmMultiBoot, more than 4 bytes means a normal radio and the
serial input switches to 100k 8E2.

## Why the CC2652 ROM boot loader can't serve

The ROM boot loader listens on UART0 (DIO2 / DIO3, the CH343) and SSI0
only, and opens only through the backdoor pin (DIO19, BOOT) at reset or when
CCFG has no valid image. The radio bay lines are elsewhere:

- radio -> module: PPM pin -> D3 -> DIO22 (UART1 RX);
- module -> radio: DIO21 (UART1 TX) -> U9 (XOR with TX1_INV, DIO24) -> D4 ->
  S.Port.

U8 puts S.Port onto DIO2 when USB is unplugged, but S.Port only carries
module -> radio during an update and DIO3 goes to the CH343 alone. The ROM
protocol (0x55 0x55 autobaud, `0x00 0xCC` acks) is not STK500 either. So the
MPM v2 has its own boot loader in flash.

## Flash layout

The CC2652R1 / RB flash is 352 KB in 8 KB sectors, CCFG in the last 88
bytes (0x57FA8, last sector at 0x56000). With `-DBOOTLOADER=ON`
(`src/linker/`, `src/bootloader/layout.h`):

| Range | Content |
|---|---|
| 0x00000 - 0x01FFF | Boot loader (sector 0), 3.9 KB today |
| 0x02000 - 0x55FFF | Application (336 KB), vector table at 0x2000 |
| 0x56000 - 0x57FFF | Last sector: CCFG (boot loader image), never written by the boot loader |

The 8 KB sector matches the STM32 layout, where EdgeTX already writes from
byte 0x2000. CCFG keeps `IMAGE_VALID_CONF` at 0 (the boot loader) and the
backdoor on DIO19, so USB recovery through the ROM boot loader always works.

**Image header.** The reserved vector table entries 7 to 10 (0x201C) hold
magic `MPV2`, length, CRC32 (zlib, header read as zeros) and a `checked`
word. `tools/fw_image.py` fills them in after `objcopy` (`firmware.bin`, the
file the J-Link flashes) and writes `mpm-v2.bin`, the file for the radio:
the image followed by the MULTI signature `multi-x00000983-<version>`
(board type 3, boot loader support and check, MULTI telemetry).

**Size.** `STK_LOAD_ADDRESS` carries a 16 bit word address: 128 KB, a limit
of STK500v1 itself (made for AVRs; avrdude reaches the ATmega2560's 256 KB
with an extra `STK_UNIVERSAL` "load extended address" command, `0x4D`,
which EdgeTX doesn't send). EdgeTX keeps the full address in a `uint32_t`
and sends only its low 16 bits, always in order, one page after the other
from 0x1000. The boot loader follows the wrap: it keeps the full address and
takes the next one forward. So the whole 336 KB can be used with no EdgeTX
change (tested with a 200 KB image). A host that goes back to the start
(a read back pass after the writes) must start a new session; reading each
page back right after writing it works.

## Power up

1. **Image check** (`checked` cleared, magic, stack pointer in SRAM, reset
   vector in the application). The CRC takes about 0.3 ms per KB (110 ms for
   336 KB), too slow for every start: it is checked once, at the first start
   after an update, then the boot loader clears `checked` (bits only go from
   1 to 0: no erase) and resets.
2. **Sync window.** STK500 on the radio link (UART1, inverted) and on the
   console (UART0, the CH343 on USB-C), 57600 8N1. The application starts:
   - after `BL_CONSOLE_MS` (20 ms) when the radio line is at the "no radio"
     level for 20 ms (`BL_RADIO_ABSENT_LEVEL`: R14 pulls DIO22 up with
     nothing on the PPM pin);
   - as soon as the radio line carries anything but `STK_GET_SYNC`: serial
     frames (100k 8E2) and PPM read as framing errors and other bytes at
     57600 (more than `BL_NOISE_BYTES`, 8);
   - at the latest after `BL_WINDOW_MS` (300 ms), when the line stays idle
     and driven.

   A radio that reboots (watchdog) with the model in the air gets the module
   back with its first frames: the window only runs its full length when the
   radio drives the line and sends nothing, which only EdgeTX's flashing
   does. EdgeTX waits 500 ms after power on before its first sync for STM32
   modules; for MPM v2 files it syncs after 10 ms, then every 100 ms
   (`mpm-v2-flash` branch), well within the window.
3. **No valid application** (none yet, interrupted update, failed CRC): the
   boot loader serves STK500 on both ports until one is written.

Measured on the MPM v2 with the bench application and no radio: 32 ms from
reset to the application answering on USB.

## Writing

- `READ_SIGN` answers `1E 55 26` (STM32 boot loader: `1E 55 AA`). EdgeTX
  (`mpm-v2-flash`) refuses an MPM v2 file for a `1E 55 AA` module and the
  other way round, before writing anything; with either signature it writes
  256 byte pages from word address 0x1000.
- **First page (0x2000) check before any erase:** the image header and
  vectors as at power up. An STM32 MULTI image (reset vector at 0x0800xxxx)
  or anything else gets `STK_FAILED`: EdgeTX reports "wrong request",
  nothing was erased and the old application still runs (tested with
  `multi-stm-1.3.4.74.bin`). The `checked` word of the first page is forced
  to 0xFFFFFFFF.
- Each page is programmed and compared; a sector is erased when its first
  page arrives. Writes outside 0x2000 - 0x55FFF are refused.
- `LEAVE_PROGMODE`: the boot loader resets when the image CRC matches, else
  keeps serving: an interrupted update leaves it in charge at every power
  up, the user flashes again (tested with half an image).
- `READ_PAGE` reads back (for host tools), `GET_SIGN_ON`, `GET_PARAMETER`,
  `SET_DEVICE`, `ENTER_PROGMODE` are answered for other STK500 hosts.
- 57600 bauds: 56 KB in 11 s (21 s with read back), 336 KB would take about
  70 s.

## Link polarity

The radio sends inverted on the PPM pin and reads S.Port inverted. The boot
loader inverts DIO22 in the I/O controller. S.Port is only driven once a
sync came from the radio: TX1_INV (DIO24) set first, so that U9 holds S.Port
low (the idle level of inverted serial) before UART1 takes DIO21 over; the
`board_init()` levels (S.Port released) are back before the application
starts. Untested on a radio.

## Internal modules

An internal MPM v2 has no USB, but EdgeTX gives the PC a way to it:

- The STK500 update works the same over the internal module UART (EdgeTX:
  duplex, 57600 8N1, normal polarity): the board's `BL_RADIO_*` settings
  name the UART and its polarity. The boot loader code assumes UART1 for the
  radio and UART0 for the console today; a board with its radio link on
  UART0 needs that generalised.
- **Recovery through the ROM boot loader.** EdgeTX's CLI (USB serial) has
  `set rfmod 0 power on|off`, `set rfmod 0 bootpin 0|1` (the internal
  module's `INTMODULE_BOOTCMD_GPIO`, wired on a dozen targets) and
  `serialpassthrough rfmod 0 <baud>` (8N1, normal polarity). If the
  internal module's UART goes to DIO2 / DIO3 (UART0, the ROM boot loader
  pins) and its BOOTCMD to DIO19 (backdoor; the CCFG `BL_LEVEL` sets its
  active level, low today: it must be the level the radio's pin takes only
  when asked, `INTMODULE_BOOTCMD_DEFAULT` is 0 on some targets), a PC can
  hold BOOT, power cycle the module and run
  the ROM boot loader protocol through the passthrough (`cc2538-bsl` style),
  whatever is in the flash. No new code in EdgeTX.
- The same passthrough also reaches the flash boot loader:
  `stk500_flash.py --no-reset` after a module power cycle.

So for an internal module: radio UART on DIO2 / DIO3, BOOTCMD on DIO19.

## Tools

```sh
cmake -S . -B build-bl -G Ninja -DCMAKE_BUILD_TYPE=Release -DBENCH=ON -DBOOTLOADER=ON
ninja -C build-bl                     # bootloader.elf, firmware.bin (with header), mpm-v2.bin
ninja -C build-bl flash               # J-Link: boot loader + application
tools/stk500_flash.py build-bl/mpm-v2.bin --verify   # USB-C: as EdgeTX does it
```

`stk500_flash.py` resets the CC2652 through the CH343 DTR (BOOT high) and
syncs every 10 ms, within the 20 ms console window. With `--no-reset` it
syncs for 5 s like EdgeTX; `--invert` drives the radio bay through a plain
USB serial adapter (TX to the PPM pin, RX from S.Port).

## Not chosen

- **The check in the application**, as `pollBoot()` on the STM32: no window
  in the boot loader, but a broken application can't be updated any more,
  and an internal module has no other way in.
- **Staging in the external NOR flash** (MX25R8035, 1 MB): write the image
  there, check it, then copy. All-or-nothing, but the boot loader is never
  overwritten and an interrupted update is not fatal anyway.
- **A different update protocol in EdgeTX** (the TI ROM protocol, or an
  ELRS-like one): the ROM boot loader isn't on the module bay pins, and
  STK500 does the job.

## To test with a radio

Everything above was tested over USB only. The module bay path needs a
radio, or the radio simulator planned for the bench (below). What to test,
and what may go wrong:

1. **Sync over the module bay** (EdgeTX `mpm-v2-flash`, "Flash External
   Multi" with `mpm-v2.bin`).
   - *RX polarity:* DIO22 is inverted in the I/O controller on the
     assumption that the radio's soft serial idles low on the PPM pin. If
     the bytes arrive wrong, no sync, and the window ends on "noise".
   - *S.Port polarity through U9:* the reasoning is that TX1_INV = 1 makes
     U9 pull S.Port low at idle (D4) and R22 pull it up for the high bits.
     If D4 / R22 work the other way, EdgeTX reads nothing ("no response")
     or garbage. Check the S.Port level on a scope before and after the
     sync.
   - *S.Port drive strength:* R22 against the radio's S.Port input
     (pull-down?) decides the high level and the edge times at 57600.
2. **Window timing with a real power up.** EdgeTX powers the module up
   with its TX line already idle, waits 10 ms, then syncs every 100 ms: the
   power rail rise, the ROM boot and `board_init()` (HPOSC) must leave the
   first or second sync inside the 300 ms window.
   - *A glitch at power up* (the line or the module rail rising) reads as
     one or two bytes: below `BL_NOISE_BYTES`, but to be checked.
3. **No radio and radio not sending.** `BL_RADIO_ABSENT_LEVEL` assumes DIO22
   reads high (R14) with nothing on the PPM pin, and low (radio soft serial
   idle) with a radio. Check both levels, and the time to the application:
   - module on USB alone (bench): 20 ms window;
   - radio powering the module before it sends (EdgeTX boot): the window
     runs until the first frames, at most 300 ms;
   - radios whose PPM pin idles high or floats while the radio boots: the
     module starts after 20 ms, so a flash right after that has to power
     cycle again (EdgeTX does).
4. **Radio reboot with frames running** (watchdog, model in the air): does
   the radio cut the module power? If so, time from the first frame to the
   module sending RF again. Expected: the window ends on the first frame
   (8 bytes of noise), plus the application's start.
5. **PPM input** instead of serial: PPM edges must count as noise (framing
   errors). A PPM train with long gaps could end the window late only if
   it looks idle for 300 ms, which it can't.
6. **The whole update from the radio:** 56 KB and a large image (200 KB,
   past the 16 bit address), EdgeTX's progress and messages, then the new
   application running. Also: a wrong file (STM32 `.bin` on the MPM v2,
   MPM v2 `.bin` on an STM32 MULTI-Module, refused by EdgeTX before
   writing), and pulling the module out mid-update (boot loader in charge
   at the next power up, flash again).
7. **Internal module** (a later board): the same over the duplex UART at
   normal polarity, and the recovery through `serialpassthrough` and
   `bootpin` (see Internal modules).

## Radio simulator

`tools/bench/baysim/` (C, pico-sdk) turns an RP2040-Zero into the radio's
external bay: GP12 to the PPM pin (bay pin 1), GP13 from S.Port (pin 5),
GP11 a high-side switch on the module supply (pin 3, as the radios'
`modulePortSetPower()`), USB-C of the MPM v2 unplugged (it would power the
module through D2). Two USB serial ports: a bridge to the bay lines, both
inverted in the GPIO block, at the host's line settings (so
`stk500_flash.py` and `radio_sim.py` run through it unchanged), and a
control port: power, polarity, the PPM line driven or left floating (no
radio), PPM instead of serial (a PIO state machine, 1 us steps), pad
levels, and a trace of the bytes both ways with microsecond
times. `tools/bench/baysim.py` runs the tests below.

Both lines are safe for the RP2040's 3.3 V: the radio only pulls the PPM
pin low (D3, R14 to 3.3 V on the module side) and the module only pulls
S.Port low (D4, R22 to 3.3 V).

## Tested over the module bay

With baysim (2026-10-20), boot loader and bench application from
`build-bl`; times from the module's power on:

- **Update** (points 1 and 6): `baysim.py flash`, EdgeTX's sequence
  (power off 3 s, sync every 100 ms): the sync at 88 ms is answered 0.5 ms
  later, then 118912 bytes in 44.3 s, 465 pages read back and verified;
  the new application runs (J-Link: PC in `bench_loop`). So the S.Port
  polarity through U9 and D4 is right: idle low with TX1_INV set, and
  EdgeTX's inverted reading at 57600 gets the replies.
- **Power up** (point 2): S.Port shows two breaks, 1.8 and 5.5 ms after
  power on (the line rising with R22 before the boot loader holds it low);
  EdgeTX only listens after its sync. The first sync answered: 7 ms after
  power on (none at 5 ms); EdgeTX's first, at 10 ms, is in.
- **Window** (point 3; the sync's time from the baysim trace): PPM line
  driven at idle (a radio): syncs answered from 7 to 290 ms, none from
  310 ms; PPM line floating (no radio, R14 high): answered at 11.7 ms, none
  from 26.6 ms (20 ms after the boot loader starts).
- **The first reply** to a sync 15 to 20 ms after power on reads
  `11 f1` (framing error) about once in five at baysim, instead of
  `14 10`: `radio_tx_on()` takes S.Port from released to idle a few
  microseconds before the start bit, and the receiver, out of the released
  line's break, misses it. A radio's next sync (100 ms later) gets a clean
  reply. A short idle time before the first reply would fix it.
- **Serial frames** (point 5): MULTI frames (100k 8E2) from 5 ms after
  power on, then a sync at 60 to 290 ms: never answered, the frames end the
  window. PPM (EdgeTX's 8 channels at 1500, 22.5 ms, 300 us pulses) from
  before power on, then a sync: answered up to 21.6 ms, none from 26.6 ms,
  both polarities (negative PPM idles at the "no radio" level, positive
  PPM's pulses count as noise): the application starts after the boot
  loader's shortest window.
- **PPM input** (`build-mpm`, `baysim.py ppm`, the MPM core's state read
  over J-Link): detected and decoded, 4 to 16 channels, both polarities,
  the widths exact to the microsecond (500 reads over 11 s, about 30
  wraps of the capture counter: no glitch), EdgeTX's -100/0/+100% widths
  (988, 1500, 2012 us) to 204, 1024, 1844; 3 channels: no input. FrSky X2
  with the X8R: TX_LQI 100, RX RSSI 91 to 94. Serial to PPM and back: about
  1 s (the input timeout), the X8R back 4 s later (UNI-RX's hold). Serial
  after the detection: the first status 84 to 90 ms after power on (75 to
  82 before, one frame more).
- **The application on the bay** (`build-mpm`: MPM core, boot loader
  layout, flashed over the bay: 80476 bytes in 29.9 s): `radio_sim.py -p
  <baysim link> status` gets the status telemetry every 500 ms through
  S.Port.
- **Radio reboot** (point 4): `baysim.py reboot`, frames every 7 ms all
  along, the module off for 1 s: its first telemetry (the status of the
  protocol start) 31 to 35 ms after power on (5 rounds).

- **A receiver over the bay** (X8R with UNI-RX, about 1 m away, on its own
  5 V): bound from the MPM v2 through the bay (FrSky X2, sub 0), then
  telemetry back through S.Port: TX_LQI 100, the receiver's RSSI 94 to 98,
  the module's 194 to 216, A1 93 to 95. Radio reboot with it (`baysim.py
  reboot --proto 64`): the module's first status 75 to 82 ms after power on
  (FrSky X's start, 31 to 35 ms with the test protocol), the receiver's
  telemetry back after 4.6 to 4.9 s (0.3 s off), 3.8 to 4.0 s (1 s off),
  0.12 to 0.34 s (6 s off): about 5 s after the cut whatever the time off.
  UNI-RX keeps the lost link's timing 4 s after its failsafe (manual), and
  a restarted protocol has a new random channel skip and hop index (the
  STM32 MULTI-Module's too): it is only found by the search after that. A
  plain signal loss (the hop sequence going on) is back in 0.2 to 0.4 s
  ([mpm-core.md](mpm-core.md#real-receivers)).

Not yet: the 200 KB image over the bay (tested over USB), an internal
module's polarity (`telem_invert` clear). A real radio is still needed for the EdgeTX side (`mpm-v2-flash`)
and the radios' own line levels.

## Next

1. A real radio.
2. Make `-DBOOTLOADER=ON` the default for the MPM v2 once the module bay
   path works.
3. Generalise the radio port (UART0 or UART1, polarity) for internal
   module boards.
