# Updating the MPM v2 firmware from the radio

The update the STM32 MULTI-Module has: the user picks a `.bin` on the SD
card ("Flash External Multi" or "Flash Internal Multi" in the SD manager)
and EdgeTX writes it through the module bay, no USB and no tools.

Status: the boot loader (`src/core/bootloader.c`, the chip's part in
`src/platforms/<family>/bl_port.c`, always built, both applications run
behind it) works over USB with a host tool that does what
EdgeTX does; the EdgeTX side is on branch `mpm-v2-flash` (not pushed;
`mpm-v2-flash-2.12` on EdgeTX 2.12). From a radio: one update on a TX15 over
the external bay (2026-10-21), one on a GX15 with the boot loader without
system reset (2026-10-24, see [Tested on a radio](#tested-on-a-radio)); the
rest of [To test with a radio](#to-test-with-a-radio) is still open.

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
3. **Link.** 57600 8N1. The radio sends on the PPM pin (bay pin 1) and
   receives on S.Port, both `ETX_Pol_Inverted`: idle high on the bay (the
   bay's "normal" is idle low). Internal modules use the duplex internal
   USART instead.
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
bytes (0x57FA8, last sector at 0x56000). With the boot loader
(`src/boards/mpm_v2/`: `app.ld`, `bootloader.ld`;
`src/platforms/cc26xx/bl_layout.h`):

| Range | Content |
|---|---|
| 0x00000 - 0x01FFF | Boot loader (sector 0), 4.2 KB today |
| 0x02000 - 0x55FFF | Application (336 KB), vector table at 0x2000 |
| 0x56000 - 0x57FFF | Last sector: CCFG (boot loader image), never written by the boot loader |

The 8 KB sector matches the STM32 layout, where EdgeTX already writes from
byte 0x2000. CCFG keeps `IMAGE_VALID_CONF` at 0 (the boot loader) and the
backdoor on DIO19, so USB recovery through the ROM boot loader always works.

The CC2755 LaunchPad (`src/boards/cc2755p10_lp/`, `src/platforms/cc27xx/
bl_layout.h`) has the same boot loader and the applications at 0x2000 too:
2 KB sectors, the boot loader in sectors 0 to 3 (2.8 KB), the application
up to 0xE8000 (the HSM firmware from there, never written), the CCFG at
0x4E020000 outside the main flash, in the boot loader's image only, its
`pAppVtor` the boot loader's vector table. No flash ECC: the `checked`
word is programmed again as on the CC2652.

**Image header.** Right after the application's vector table (0x20D8 on the
CC2652: `src/core/app_image.h`, `app_header.c`, the `.img_hdr` section of
`src/linker/sections.ld`): magic `MPMI`, chip (`0x2652`), length, CRC32
(zlib, header read as zeros) and a `checked` word. The boot loader and
`tools/fw_image.py` find it by its magic in the first 512 bytes, as EdgeTX's
boot loader finds its version tag after the vector table: no chip's reserved
vector entries are used (the CC27xx's entry 7 is its SecureFault handler).
The scan takes 14 us at power up (CC2652 LaunchPad, cold flash cache). The
linker gives the length (`__img_length`, `app.ld`), no CRC and `checked`
already 0: `firmware.bin` (the file the J-Link flashes) and
`mpm-v2-full.bin` / `.hex` (objcopy: the application added to
`bootloader.elf` at 0x2000) start without a CRC check.
Python only makes the file for the radio, `tools/fw_image.py`:
`mpm-v2.bin`, the image with its CRC and `checked` 0xFFFFFFFF, followed by
the MULTI signature `multi-x00000983-<version>` (board type 3, boot loader
support and check, MULTI telemetry). Without Python 3 the build leaves it
out.

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

1. **Image check** (`checked` cleared, header for this chip, stack pointer
   in SRAM, reset vector in the application). The CRC takes about 0.3 ms
   per KB (110 ms for 336 KB), too slow for every start: it is checked
   once, at the first start after an update, then the boot loader clears
   `checked` (bits only go from 1 to 0: no erase; flash cache off for the
   write, then invalidated) and goes on. No system reset there: with a
   debugger attached, the ROM boot after one waits in a `wfi` for a debug
   halt (FWFLAG bit 2, what sets it is not known), which left the module
   stuck at its first start after a J-Link flash.
2. **Sync window.** STK500 on the radio link (UART1, idle high) and on the
   console (UART0, the CH343 on USB-C), 57600 8N1. The application starts:
   - after `BL_CONSOLE_MS` (20 ms) when the radio line is at the "no radio"
     level for 20 ms (`BL_RADIO_ABSENT_LEVEL`, a board setting). Not on the
     MPM v2: R14 pulls DIO22 up with nothing on the PPM pin, the level of
     the radio's idle line, so the window runs its full length on the USB
     bench too;
   - as soon as the radio line carries anything but `STK_GET_SYNC`: serial
     frames (100k 8E2) and PPM read as framing errors and other bytes at
     57600 (more than `BL_NOISE_BYTES`, 8);
   - at the latest after `BL_WINDOW_MS` (300 ms), when the line stays idle
     and driven.

   On the LaunchPads (CC2652, CC2755) the radio link is UART0 itself, the
   XDS110 back channel, where their MPM port talks to the radio
   (`radio_sim.py`): `BL_RADIO_UART0` gives them the same window, 300 ms on
   the idle line, the application at once on radio frames.

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
  256 byte pages from word address 0x1000. The signature names this boot
  loader, not the chip: the CC2755's answers the same, and the image header
  tells the chips apart.
- **First page (0x2000) check before any erase:** the image header (for this
  chip) and vectors as at power up. An STM32 MULTI image (reset vector at
  0x0800xxxx), an image for another chip, one with the header of the boot
  loaders before November 2026 (in the vector table) or anything else gets
  `STK_FAILED`: EdgeTX reports "wrong request", nothing was erased and the
  old application still runs (tested with `multi-stm-1.3.4.74.bin`, and with
  an old-header v0.4.0 image on the MPM v2 and the LaunchPad). The `checked`
  word of the first page is forced to 0xFFFFFFFF.
- **Boot loaders before November 2026** (releases up to v0.4.0) look for the
  header in the vector table and refuse the newer images the same way: such
  a module takes `mpm-v2-full.bin` once over USB-C (`tools/rom_flash.py`,
  boot loader and application), then updates from the radio again.
- Each page is programmed and compared; a sector is erased when its first
  page arrives. Writes outside 0x2000 - 0x55FFF are refused.
- `LEAVE_PROGMODE`: the boot loader starts the application when the image
  CRC matches (cache back on, `checked` cleared, no system reset), else
  keeps serving: an interrupted update leaves it in charge at every power
  up, the user flashes again (tested with half an image).
- `READ_PAGE` reads back (for host tools), `GET_SIGN_ON`, `GET_PARAMETER`,
  `SET_DEVICE`, `ENTER_PROGMODE` are answered for other STK500 hosts.
- 57600 bauds: 56 KB in 11 s (21 s with read back), 336 KB would take about
  70 s.

## Link polarity

EdgeTX names polarities after its own UART: on the bay, through the
radio's inverters, `ETX_Pol_Normal` idles low and `ETX_Pol_Inverted` idles
high. For the update it sends on the PPM pin and reads S.Port, both
inverted (`multi_firmware_update.cpp`): idle high. Its MULTI frames are
inverted too (idle high on the PPM pin, seen on a TX15 on 2026-10-21), but
it reads their telemetry on S.Port normal (idle low, as FrSky S.Port).

The boot loader (`BL_RADIO_INVERTED 0`) reads DIO22 as is. S.Port is only
driven once a sync came from the radio: TX1_INV (DIO24) cleared, so U9
passes the UART's idle level (high, released through D4, R22 pulls it up)
before UART1 takes DIO21 over; the `board_init()` levels (S.Port released)
are back before the application starts. The application reads DIO22 as is
and drives S.Port idle low (TX1_INV set) for the frames' telemetry.

Until 2026-10-21 both were the other way round (DIO22 inverted, the boot
loader's S.Port idle low), as baysim drove them: the TX15's frames came in
as framing and parity errors, so the module never left "no input".
The update with these levels is untested on a radio.

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

An external module has its own USB-C, but the radio's USB works too with an
EdgeTX patch (EdgeTX PR #7927 on main, branch `cli-ext-module-passthrough`;
the same commits on 2.12 in `mpm-v2-passthrough-2.12`): EdgeTX 2.12 and
main only pass through to the internal module
(`// TODO: external module` in `cliSerialPassthrough()`).
`serialpassthrough rfmod 1 <baud> ppm` opens the bay as "Flash external
Multi" does (TX on the PPM pin, RX on S.Port, both
idle high, softserial if needed), powers the bay off for 500 ms and on again
with the ports open, and passes bytes until the radio's USB is unplugged,
the host sends a break, or, with `hangup` as the last argument, the host
closes the port (DTR drop); then the bay is powered off and the pulses come
back. `stk500_flash.py --radio` sends the command with `hangup` and syncs
every 10 ms: the boot loader answers in its 300 ms window, and the
pass-through ends when the script exits.

## Tools

```sh
cmake -S . -B build -G Ninja
ninja -C build                        # bootloader.elf, firmware.bin (with header), mpm-v2.bin, ...
ninja -C build flash-firmware         # J-Link: boot loader + application (flash-bench: the bench)
tools/stk500_flash.py build/mpm-v2.bin --verify   # USB-C: as EdgeTX does it
```

`mpm-v2-full.bin` / `.hex` (also built) hold the whole flash: boot
loader, application, CCFG. `tools/rom_flash.py` writes it over USB-C through
the ROM boot loader, for the first install or a recovery (no debugger; see
the README's "Trying it"). Tested on the MPM v2 (2026-10-20): internal
flash erased over J-Link, USB-C the only power, `rom_flash.py` 13.5 s at
115200 baud (4.4 s at 460800, the default), the application up with its
file system; then `stk500_flash.py --verify` over it, 48 s.

`stk500_flash.py` resets the CC2652 through the CH343 DTR (BOOT high) and
syncs every 10 ms, within the 20 ms console window. On a LaunchPad
(`--openocd "openocd -f board/ti_cc26x2_launchpad.cfg"`, plus `-c "adapter
serial ..."` with several XDS110) OpenOCD resets the chip and leaves it
halted, the sync starts, and a second OpenOCD lets it run (`halt`, then
`resume`: the CC27xx's second OpenOCD finds the core in an unknown state,
where `resume` alone does nothing): a boot loader already in a session
would answer a sync sent before the reset. Syncs sent
while OpenOCD drives the XDS110 did not reach the 20 ms console window,
hence the LaunchPad's 300 ms one. Tested on the CC2652 LaunchPad
(2026-10-31): updates both ways between the firmware and the bench, an
old-header image refused, half an image (the boot loader then stays) and
the full one over it. The same on the CC2755 LaunchPad, the bench and the
MPM port (`cc27-*-update.bin`), and a CC2652 image refused there.

With `--no-reset` it syncs for 5 s like EdgeTX; `--invert` drives the
radio bay through a plain USB serial adapter (TX to the PPM pin, RX from
S.Port); `--radio` goes through a radio's USB serial (EdgeTX CLI, see
[Internal modules](#internal-modules) for the external bay).

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
   - *RX polarity:* DIO22 read as is: EdgeTX's inverted soft serial idles
     high on the PPM pin, as its MULTI frames do on a TX15. If the bytes
     arrive wrong, no sync, and the window ends on "noise".
   - *S.Port polarity through U9:* TX1_INV = 0 lets the UART's idle level
     through, released by D4 and pulled up by R22: idle high, what
     EdgeTX's inverted reading expects. If EdgeTX reads nothing ("no
     response") or garbage, check the S.Port level on a scope before and
     after the sync.
   - *S.Port drive strength:* R22 against the radio's S.Port input
     (pull-down?) decides the high level and the edge times at 57600.
2. **Window timing with a real power up.** EdgeTX powers the module up
   with its TX line already idle, waits 10 ms, then syncs every 100 ms: the
   power rail rise, the ROM boot and `board_init()` (HPOSC) must leave the
   first or second sync inside the 300 ms window.
   - *A glitch at power up* (the line or the module rail rising) reads as
     one or two bytes: below `BL_NOISE_BYTES`, but to be checked.
3. **No radio and radio not sending.** No `BL_RADIO_ABSENT_LEVEL` on the
   MPM v2: DIO22 reads high (R14) with nothing on the PPM pin, and high
   (the radio's idle line) with a radio. Check the time to the application:
   - module on USB alone (bench): the full 300 ms window;
   - radio powering the module before it sends (EdgeTX boot): the window
     runs until the first frames, at most 300 ms;
   - radios whose PPM pin idles high or floats while the radio boots: the
     module starts after 20 ms, so a flash right after that has to power
     cycle again (EdgeTX does).
4. **Radio reboot with frames running** (watchdog, model in the air): does
   the radio cut the module power? If so, time from the first frame to the
   module sending RF again. Expected: the window ends on the first frame
   (8 bytes of noise), plus the application's start.
   *Tested* (TX15, EdgeTX 2.12, 2026-10-21): a special function on a
   switch runs `while(1){}` in the mixer task, so the frames stop and,
   the mixer no longer resetting it, the watchdog resets the radio. The
   module keeps sending while the frames stop (input lost only changes
   the LED), then reboots with the radio: the bay's power is cut (its log
   starts again). FlySky received by the 4-in-1 on one hop
   channel: 1.45 s without packets from the reset to the first packet.
   Application: frames detected 36 ms after its start, FlySky at 43 ms;
   the rest is the radio's boot, the bay power-up and the boot loader
   window, not split yet (Saleae on the bay's power and PPM pins).
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
module through D2). Two USB serial ports: a bridge to the bay lines, each
line's polarity set in the GPIO block as a radio's (`invert TX RX`): the
PPM pin idle high, S.Port idle low for MULTI telemetry (the default) and
high for the boot loader's replies (`baysim.py flash` and `window`; until
baysim 3, 2026-10-21, both lines idled low, see
[Link polarity](#link-polarity)), at the host's line settings (so
`stk500_flash.py` and `radio_sim.py` run through it unchanged), and a
control port: power, polarity, the PPM line driven or left floating (no
radio), PPM instead of serial (a PIO state machine, 1 us steps), pad
levels, and a trace of the bytes both ways with microsecond
times. `tools/bench/baysim.py` runs the tests below.

Both lines are safe for the RP2040's 3.3 V: the radio only pulls the PPM
pin low (D3, R14 to 3.3 V on the module side) and the module only pulls
S.Port low (D4, R22 to 3.3 V).

## Tested on a radio

TX15, EdgeTX 2.12 with the `mpm-v2-flash` patch (branch `mpm-v2-flash-2.12`),
the MPM v2 in its external bay (2026-10-21): "Flash external Multi" with
`mpm-v2.bin` from the SD card's `FIRMWARE` folder, over an application
flashed before with the J-Link (4 bytes longer, so that the result tells
the two apart): flashed successfully. Read back over the J-Link: the
application area is `firmware.bin` byte for byte except `checked`, cleared
by the boot loader after the CRC; the old image's last bytes overwritten by
EdgeTX's 24 byte MULTI signature (it writes the whole file, past the
image's length: harmless). The new application then runs (serial at 37 ms,
EdgeTX's protocol scan, FlySky). Points 1 and 6 for a 126 KB image; not
yet: an image past 128 KB, a wrong file, a pulled module.

GX15, EdgeTX with the `mpm-v2-flash` patch, the MPM v2 in its external bay
with the J-Link attached (2026-10-24, `7f133c2`: the boot loader without
system reset, image header from the linker): "Flash external Multi" with
`mpm-v2.bin` (85020 bytes) over the bench application. Read back over the
J-Link: header length 84996, CRC 708a96fd, `checked` 0 (the CRC checked at
the first start, then cleared), the application area `firmware.bin` but for
the CRC word, the MULTI signature after it; the application running (PC in
flash), not in the ROM boot's `wfi` as with the system reset of the earlier
boot loader. EdgeTX then shows the module's status and protocol list.
Then the `v0.3.0` file the same way (`mpm-v2.bin`, CRC 53b21452): EdgeTX
shows V2.3.0.3; header CRC 53b21452, `checked` 0, the application running.
(An openocd `verify_image` on the running board loads its checksum code
into SRAM and leaves the CPU there: a reset over the J-Link after it.)

Through the radio's USB (EdgeTX 2.12 with the pass-through patch, branch
`mpm-v2-passthrough-2.12`, CLI on the USB serial):
`stk500_flash.py build-mpm/mpm-v2.bin --port /dev/cu.usbmodem... --radio
--verify` synced in the first power-up window, 504 pages written and read
back in 61 s. After unplugging the radio's USB the application ran (FlySky
on all 16 hops, received by the 4-in-1).

## Tested over the module bay

With baysim (2026-10-20), boot loader and bench application from
`build-bl`; times from the module's power on:

- **Update** (points 1 and 6): `baysim.py flash`, EdgeTX's sequence
  (power off 3 s, sync every 100 ms): the sync at 88 ms is answered 0.5 ms
  later, then 118912 bytes in 44.3 s, 465 pages read back and verified;
  the new application runs (J-Link: PC in `bench_loop`). With baysim's
  levels, idle low on both lines, which a real radio doesn't use (see
  [Link polarity](#link-polarity)): the update is to test again.
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
2. Generalise the radio port (UART0 or UART1, polarity) for internal
   module boards.
