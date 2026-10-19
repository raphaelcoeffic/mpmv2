# Updating the MPM v2 firmware from the radio

Status: proposal, nothing implemented yet. Today the MPM v2 is flashed over
USB-C through the CC2652 ROM boot loader (`tools/serial_bl.py`, BOOT on DIO19
driven by the CH343 DTR/RTS) or with a J-Link on J10.

The goal is the update the STM32 MULTI-Module already has: the user picks a
`.bin` on the SD card ("Flash External Multi" in the SD manager) and EdgeTX
writes it through the module bay, no USB and no tools.

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
2. **Reset.** `pulsesStop()`, module bay power off, 3 s, power on, 500 ms.
   On radios that can't switch the module power the module keeps running.
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
MPM v2 needs its own boot loader in flash.

## Proposal

### Flash layout

The CC2652R1 / RB flash is 352 KB in 8 KB sectors, CCFG in the last 88
bytes (0x57FA8, last sector at 0x56000).

| Range | Content |
|---|---|
| 0x00000 - 0x01FFF | Boot loader (sector 0) |
| 0x02000 - 0x55FFF | Application, vector table at 0x2000 |
| 0x56000 - 0x57FFF | Last sector: application tail and CCFG, never written by the boot loader |

The 8 KB sector matches the STM32 layout: with signature `1E 55 AA`, EdgeTX
already writes from byte 0x2000, so no EdgeTX change is needed for the
addresses. CCFG keeps `IMAGE_VALID_CONF` at 0 (the boot loader) and the
backdoor on DIO19, so USB recovery always works.

**Size.** `STK_LOAD_ADDRESS` carries a 16 bit word address: 128 KB, a
limit of STK500v1 itself (made for AVRs; avrdude reaches the ATmega2560's
256 KB with an extra `STK_UNIVERSAL` "load extended address" command,
`0x4D`, which EdgeTX doesn't send). EdgeTX keeps the full address in a
`uint32_t` and sends only its low 16 bits, always in order, one page after
the other from 0x1000. So the boot loader follows the wrap itself: it keeps
its own write address, checks that the low 16 bits match it, and carries on
past 0xFFFF. With that the application can use all of 0x2000 - 0x55FFF
(336 KB, today about 57 KB) with no EdgeTX change. If EdgeTX ever sends
pages out of order or skips some, the extended address command is the
clean fix on its side.

### Entering the boot loader

As on the STM32 MULTI-Module:

1. **From the application.** After power up the application listens on
   UART1 at 57600 before the serial / PPM input detection
   (`detect_input_signal()`). `0x30 0x20` and 5 ms of silence: it writes a
   magic word to a RAM location kept across a reset (a `.noinit` word, or an
   AON register) and resets. EdgeTX waits 500 ms after power on, then
   repeats `GET_SYNC` for 5 s, so the boot loader answers the next one. Any
   other traffic: normal start, no delay added to the boot.
2. **In the boot loader.** No magic and a valid application: jump to it at
   once (VTOR = 0x2000, stack pointer and reset vector from 0x2000). Magic
   set, or no valid application: stay and serve STK500 on UART1 with no
   timeout.

Radios that don't switch the module power never reset the module: the
application would have to spot the sync while running (frames stop after
`pulsesStop()`, then bytes at 57600 arrive as framing errors at 100k 8E2).
To be checked on such a radio.

### Writing

- `READ_SIGN` answers `1E 55 AA`.
- Each `PROG_PAGE` is checked, then programmed; a sector is erased when its
  first page arrives. Erase (about 8 ms for 8 KB) and program fit well within
  the 100 ms EdgeTX waits for `STK_OK`.
- **Address:** the boot loader keeps its own write address (byte 0x2000,
  then +256 per page) and accepts a `LOAD_ADDRESS` only when the low 16 bits
  of its word address (byte address / 2) match (see Size above).
- **First page (0x2000) check before any erase:** initial stack pointer in
  the CC2652 SRAM (0x20000000 - 0x20014000), reset vector inside the
  application range. An STM32 MULTI image (reset vector at 0x0800xxxx) or
  anything else gets `STK_FAILED`: EdgeTX reports "wrong request", nothing
  was erased and the old application still runs.
- Writes below 0x2000 or into the CCFG sector are refused the same way.
- **Valid marker last.** The application is valid when a CRC32 over its
  image matches. The boot loader computes it after `LEAVE_PROGMODE` and
  writes the marker, or the build places length and CRC in a header right
  after the vector table and the boot loader checks it at every start (no
  extra flash write, and an interrupted update fails the check). Either way,
  an interrupted update leaves the boot loader serving STK500 at the next
  power up: the user simply flashes again.

### UART polarity

The radio sends inverted on the PPM pin and reads S.Port inverted, the same
as the normal serial input and telemetry. The boot loader sets the same
inversion as the application: DIO22 inverted in the I/O controller, TX1_INV
(DIO24) set for U9 so that S.Port idles as the radio expects, DIO21 to U9 as
in telemetry. The pin levels at boot (`board_init()`) apply to the boot
loader too: RF switch, SKY66112 and SX1281 NSS left safe.

### The `.bin`

The build appends the 24 byte MULTI signature to the application `.bin`
(linked at 0x2000). Two options:

- **No EdgeTX change:** flags with board type STM (1), boot loader support
  and check, MULTI telemetry. Works on every EdgeTX today; the boot
  loader's first page check is the only protection against the wrong file.
- **Board type 3 for the CC2652:** EdgeTX would accept it as an external
  MULTI file and could compare the file's board type with the device
  signature (`1E 55 AA` is shared by the STM32 boot loader today, so the
  MPM v2 would answer a signature of its own, e.g. `1E 55 CC`, with 256 byte
  pages from 0x1000 too). A wrong file is then refused before the module is
  touched. Needs an EdgeTX change.

Recommended: start with the first, propose the second to EdgeTX once the
boot loader works.

### Not chosen

- **Staging in the external NOR flash** (MX25R8035, 1 MB): write the image
  there, check it, then copy. It makes the update all-or-nothing, but the
  boot loader is never overwritten and USB recovery exists, so an
  interrupted update is not fatal anyway. More code for little gain.
- **The application writing its own flash:** no boot loader, but a bad
  image means no update path but USB.
- **A different update protocol in EdgeTX** (the TI ROM protocol, or an
  ELRS-like one): needs the ROM boot loader on other pins, which it can't do,
  or new EdgeTX code with no gain over STK500.

## Steps

1. Link the application at 0x2000 (`cc26x2r1f.lds` copy, VTOR set early)
   and check it still runs and still flashes over USB / J-Link (the J-Link
   flashes boot loader and application together).
2. Boot loader: UART1 STK500 subset with the address wrap, flash erase / program (driverlib
   `FlashSectorErase()` / `FlashProgram()` from ROM), first page and range
   checks, CRC, jump.
3. Host test without a radio: a Python STK500 sender doing exactly what
   EdgeTX does (timings and padding included) through a USB serial adapter
   on the bay pins, inverted.
4. Application: the 57600 sync check at power up, magic, reset.
5. Signature appended by the build (`tools/` script after `objcopy`).
6. Test from a radio with EdgeTX: external bay, then a radio without module
   power switching.
