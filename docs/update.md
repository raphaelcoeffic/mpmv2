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

## Radio simulator (planned)

Until then the bench rig is the test bed. A radio simulator on an
RP2040-zero will stand in for the radio:

- the module bay lines: inverted serial out on the PPM pin, inverted serial
  in from S.Port, normal polarity for an internal module's UART;
- a high-side switch on the module power, as the radios have
  (`modulePortSetPower()`), to time power ups;
- EdgeTX's flashing sequence (power off 3 s, on, sync, pages, as
  `stk500_flash.py`), and normal traffic: MULTI serial frames (100k 8E2)
  and PPM, to check the window ends on them;
- a radio reboot: frames, power off for a radio's boot time, power on,
  frames again, with the time until the module answers.

Points 1 to 6 above can then be run on the bench; a real radio is still
needed for the EdgeTX side (`mpm-v2-flash`) and the radios' own line
levels.

## Next

1. Radio simulator, then the tests above.
2. Make `-DBOOTLOADER=ON` the default for the MPM v2 once the module bay
   path works.
3. Generalise the radio port (UART0 or UART1, polarity) for internal
   module boards.
