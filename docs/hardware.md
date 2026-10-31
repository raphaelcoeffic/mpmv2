# MPM v2 hardware

Notes on the RadioMaster MPM v2 board (`doc/MPM_OUT.pdf`, rev 1.3, 2024-02-19):
how the firmware uses it, and what to change in the next revision. The pin
definitions are in `src/boards/mpm_v2/board.h`, the levels at boot in
`board_pins_init()` (`board.c` there).

## CC2652RB pins

| DIO | Net | Connected to |
|---|---|---|
| 0 | SWITCH | Button to GND, R99 pull-up |
| 2 | RX | U8 mux: CH343 TXD when USB_5V is present, else S.Port through U9 (XOR with RX_INV) |
| 3 | TX | CH343 RXD |
| 7 | FLASH_CS | MX25R8035 (R7 pull-up) |
| 8, 9, 10 | MISO, MOSI, SCLK | Flash and SX1281 (shared SPI bus) |
| 11 | DIO11_NSS | SX1281 NSS (no pull-up) |
| 13 | RF-SW | U3 RF switch: 1 = CC2652, 0 = SX1281 |
| 14 | CTX | SKY66112 TX/RX |
| 15 | ANT_SEL | SKY66112: 0 = ANT1 (J2), 1 = ANT2 (J3) |
| 16, 17 | TDO, TDI | J10 |
| 18 | LED DIN | TC2020RGB |
| 19 | BOOT | ROM boot loader backdoor: R15 pull-up, Q1 from CH343 DTR/RTS (as on ESP32 boards: DTR low and RTS high reset, DTR high and RTS low pull BOOT low, the same level on both does nothing; `tools/rom_flash.py`) |
| 21 | TX1 | U9 XOR with TX1_INV, then D4 onto S.Port (R22 pull-up) |
| 22 | RX1 | CPPM, radio bay pin 1, through D3 (R14 pull-up) |
| 23 | RX_INV | U9 2B |
| 24 | TX1_INV | U9 1B |
| 26, 27, 28 | SX1281 NRST, DIO1, BUSY | |

DIO 1, 4-6, 12, 20, 25, 29 and 30 are not connected. There is no 32 kHz
crystal and no 48 MHz crystal (HPOSC).

## RF chain

CC2652 → F2 balun → U3 RF1; SX1281 → U3 RF2. U3 → F1 band-pass → SKY66112
(PA/LNA) → J2 / J3. On the SKY66112, CSD, CRX and CHL are pulled up and CPS is
grounded: CTX alone switches between RX (LNA, 11 dB) and TX high-power mode
(22 dB, +21 dBm). CTX is driven by the RF core PA enable (RFC_GPO1). The PA
input is +5 dBm max: see `RF_TX_POWER_DEFAULT` in `src/boards/mpm_v2/board.h`
and `doc/SKY66112-11_203225P.pdf`.

With the SX1281 (`src/drivers/sx1281.c`, `doc/SX1280-81_Rev3.2_Mar2020.pdf`), U3
is switched to RF2 and CTX becomes a GPIO, high only while the SX1281 sends
(nothing on the SX1281 drives it). The SX1281 goes up to +12.5 dBm: the driver
caps it at +2 dBm into the PA (default 0 dBm, as the CC2652). Its GFSK sync
word sits right aligned in the 5 byte registers (checked on air). DIO1
signals TX done and RX done (polled as a GPIO, no interrupt); the other IRQ
flags are read over SPI.

## Radio bay and USB

As in the STM32 MULTI-Module, S.Port only goes from the module to the radio:
telemetry, and the boot loader's replies when flashing from the radio. The
radio sends on the PPM pin (bay pin 1), 100k 8E2, or 57600 8N1 for the boot
loader. On MPM v2:

- radio → module: DIO22 (UART1 RX);
- module → radio: DIO21 (UART1 TX) through U9 and D4;
- USB console and ROM boot loader: DIO2 / DIO3 (UART0, fixed pins for the ROM
  boot loader).

U8 only switches the S.Port RX path onto DIO2, which nothing uses, so USB can
stay plugged in while the radio bay is used.

## Next revision

- **Remove the inverters (U9).** The CC2652 I/O controller inverts any pin
  (`IOC_IOMODE_INV`) and has open-drain modes (`IOC_IOMODE_OPEN_DRAIN_INV`):
  S.Port TX can be DIO21 in open-drain mode with the R22 pull-up, the
  polarity set in software, and DIO22 can receive inverted serial as is. A
  series resistor or D4 can stay as protection, since the CC2652 pins are not
  5 V tolerant. This frees DIO23 and DIO24 (both analog capable).
- **Also consider removing U8** (with R3, R8, C15): MULTI never receives on
  S.Port. DIO2 would go straight to the CH343. If S.Port RX is ever needed
  (half-duplex), DIO21 can receive too.
- **Pull-up on SX1281 NSS (DIO11).** It is also SSI0_FSS of the ROM boot
  loader, which listens on SSI0 (DIO8-11) as well as UART0: a floating FSS
  or SCLK could make it pick SSI0 and ignore the UART until reset. It also
  keeps the SX1281 off the flash's bus before the firmware drives the pin.
- **The SX1281 on its own SPI bus (SSI1).** Today the flash and the SX1281
  share SSI0, which works because the MPM core doesn't use the SX1281:
  flash work runs in the main loop between callbacks, and the flash
  releases CS during its erase and program waits. With an SX1281 protocol
  (2.4 GHz LoRa / FLRC, hundreds of packets a second), a flash transfer in
  progress (a 256 byte read and its command, 0.2 to 0.3 ms) can't be
  interrupted: it would delay a TX trigger or the FIFO read after DIO1, or
  force all SX1281 SPI work into the main loop behind a bus lock. With two
  buses neither driver knows about the other, both can use DMA at once, and
  a stuck device can't block the other's bus. Three more pins: SSI1 is
  unused (`src/platforms/cc26xx/spi.c` drives both SSIs) and the I/O
  controller puts it on any DIO, nine of which are free (more without U9). A
  possible layout, to check against the TRM's ROM boot loader notes: the flash
  stays on SSI0 (DIO8-10) with its CS moved to DIO11 and pulled up (FSS high
  keeps the ROM boot loader off SSI0, the item above), the SX1281 on SSI1 on
  free pins with a pull-up on its NSS. Fallback if pins get tight: a shared
  bus with the SX1281's SPI work only in the main loop (its interrupts set a
  flag), the flash driver already working in short transfers; tight at
  high packet rates.
- **A test pad** on a free DIO, for timing measurements.
- **Battery voltage:** a divider from the bay VCC to an analog pin (DIO23-30,
  e.g. one freed by U9).
- **USB presence:** USB_5V through a divider to a free DIO, so the firmware
  knows when the console is connected.
