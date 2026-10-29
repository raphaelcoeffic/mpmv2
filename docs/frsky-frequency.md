# FrSky's frequency plans

Stock FrSky receivers listen about 25 FSCTRL0 steps (40 kHz) below the MPM
v2 at option 0 ([interop-results.md](interop-results.md), "Stock FrSky
receivers"). FrSky's own firmware shows where that comes from: their CC2500
gear and their CC26xx gear use two different frequency plans, and the CC26xx
one has a fixed offset built in.

## What FrSky's firmware programs

Read from FrSky's firmware images, October 2026:

| Firmware | Chip | FCC channel 0 | LBT channel 0 | Spacing | Per-unit correction |
|---|---|---|---|---|---|
| XJT ACCST 2.0.1 (transmitter) | CC2500 | 2403.99985 MHz (FREQ 0x5C7627) | 2405.00000 MHz (FREQ 0x5C8000) | 299926.76 Hz (MDMCFG0 0x7A) | none: FSCTRL0 0 |
| XJT-Lite 190730 (transmitter) | CC2650 | 2403.970 MHz | 2404.970 MHz | 300000 Hz | none |
| G-RX8 ACCST 2.1.9 (receiver) | CC2650 | 2403.970 MHz | 2404.970 MHz | 300000 Hz | none |

- **XJT (CC2500):** the CC2500 setup is three tables of (register, value)
  pairs, one per mode. FSCTRL0 is 0 in all of them, and no other code writes
  it. The other register writes are the per-channel FSCAL values, CHANNR,
  PATABLE, ADDR, PKTCTRL1 and MCSM0. This is the plan the MPM's FrSky code
  programs too.
- **XJT-Lite (CC2650):** the hop function takes CMD_FS's MHz and fractional
  frequency from two tables per mode in flash, 270 channels each. Set A (FCC)
  is 2403.970 MHz + channel × 300 kHz, within 6 Hz. Set B (LBT) is 2404.970
  MHz + channel × 300 kHz, except channel 0 at 2404.975. Its CCFG applies no
  crystal trim (XOSC_CAP_MOD 1).
- **G-RX8 (CC2650):** a TI-RTOS application. Its set-frequency function
  computes `f = base + channel × spacing` in whole Hz, then CMD_FS's MHz
  (`f / 1000000`) and fractional part. `base` and `spacing` are initialised
  constants, read at the two hop calls and never written: 2403970000 Hz
  (FCC) or 2404970000 Hz (LBT), and 300000 Hz.

So FrSky's CC26xx plan is the CC2500 plan rounded to the MHz, minus 30 kHz,
with a 300.000 kHz spacing instead of the CC2500's 299.927 kHz. Against the
CC2500 plan:

| Channel | CC26xx plan |
|---|---|
| 0 | −29.9 kHz |
| 117 | −21.3 kHz |
| 235 | −12.6 kHz |

That's about 13.5 FSCTRL0 steps below on average, and 19 steps at channel 0.

## Reading it

- The offset is deliberate and in the direction the measurements show: the
  CC2500 receivers (X8R, S8R, V8R4-II) and the 4-in-1's CC2500 sit 32 to
  40 kHz below the CC2500 plan's nominal frequencies. FrSky moved their CC26xx
  gear about 30 kHz down to meet their CC2500 hardware.
- The spacing difference makes FrSky's CC26xx gear drift about 17 kHz upward
  across the band relative to CC2500 gear (73 Hz per channel).
- None of these firmwares has a tune setting or a per-unit calibration.
- A tester's G-RX6 (CC2650 receiver, stock ACCST 2.1.0 LBT) needed about −25
  against both an MPM v2 at the CC2500 plan and an XJT-Lite with a non-FrSky
  firmware; with UNI on the same receiver, about 0. The plan above accounts
  for about −13.5 steps of that on average; the rest would be that unit's
  crystal.

## What the MPM v2 does

The plans are chosen in the FrSky code (`FrSkyDVX_Common.c`, to be sent
upstream), not in the CC2500 emulation, which programs whatever FREQ, CHANNR
and FSCTRL0 hold:

- **FrSky X and X2** (`FRSKY_CC26XX_PLAN`, CMake, ON by default): FrSky's
  CC26xx plan, as FrSky's own CC2650 transmitter. `FrSkyX_set_channel()`
  writes FREQ = the conf's FREQ − 76 steps (30 kHz) + channel × 756.185 steps
  (300 kHz), and CHANNR 0, instead of CHANNR; the option goes to FSCTRL0 as
  is. After the emulation's rounding to the kHz, every channel 0..235 lands on
  FrSky's frequency, FCC and LBT. The LBT channel 0 exception of the
  XJT-Lite's table isn't copied.
- **With `FRSKY_CC26XX_PLAN=OFF`:** the CC2500 plan, 25 steps lower, for
  comparing the two.
- **FrSky D and V:** always the CC2500 plan, 25 steps lower. Neither the
  XJT-Lite nor the G-RX8 implements them, so there is no FrSky CC26xx plan to
  copy; the −25 was measured on the V8R4-II.
- **The −25** is `FRSKY_FSCTRL0_OFFSET` (`src/mpm/upstream/mpm_config.h`),
  added to the option by `FrSky_fsctrl0()` (clamped to ±127) at init and by
  `FrSky_SetFreqOffset()` after an option change. All the CC26xx boards sit
  at the nominal frequency, so it's one value for all of them. FrSky L is left
  as it was.
- On the 4-in-1 (real CC2500), neither is defined: the code is the same as
  before, and the STM32 release images are byte-identical.

## How the images were read

- The `.frsk` images were descrambled with `frk_tools/unpacker.py`; the XJT
  and XJT-Lite images are the stock firmware.
- The CC2650 images were built with TI's compiler, which keeps initialised RAM
  compressed in `.cinit` records (LZSS on the G-RX8, RLE24 on the XJT-Lite).
  Decoding them gives the RAM image with the RF commands (CMD_PROP_RADIO_SETUP,
  CMD_FS, CMD_PROP_TX, CMD_PROP_RX) and the frequency variables. Literal-pool
  references to CMD_FS's frequency fields lead to the functions that write
  them, disassembled and decompiled with radare2 / r2ghidra.
