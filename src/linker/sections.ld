/*
 Sections of the boot loader and the application, from the TI cc26x2r1f.lds
 (SimpleLink SDK, BSD-3-Clause). The memory map is in bootloader.ld / app.ld.
*/

ENTRY( ResetISR )

_estack = ORIGIN(SRAM) + LENGTH(SRAM);

_Min_Heap_Size = 0;
_Min_Stack_Size = 0x100;

SECTIONS
{
    .text :
    {
        _text = .;
        KEEP(*(.resetVecs))
        *(.text*)
        *(.rodata*)
        *(.init)
        *(.fini*)
        *(.eh_frame*)
        _etext = .;
    } > FLASH = 0

   .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    .init_array (READONLY) :
    {
        _init_array = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        _einit_array = .;
    } > FLASH

    .data :
    {
        _data = .;
        *(.data*)
        _edata = .;
    } > SRAM AT > FLASH
    _ldata = LOADADDR(.data);
    /* End of what is in flash (objcopy -O binary ends there) */
    _eimage = LOADADDR(.data) + SIZEOF(.data);

    .ramVecs (NOLOAD) : ALIGN(256)
    {
    	*(.ramVecs)
    } > SRAM

    .bss :
    {
        __bss_start__ = .;
        _bss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
        __bss_end__ = .;
    } > SRAM

    ._user_heap_stack :
    {
      . = ALIGN(4);
      . = . + _Min_Heap_Size;
      . = . + _Min_Stack_Size;
      . = ALIGN(4);
    } > SRAM
}

/* Vector table entries 7 to 10: the image header in an application
   (app.ld), reserved (0) in the boot loader */
PROVIDE(__img_hdr_magic = 0);
PROVIDE(__img_hdr_length = 0);
PROVIDE(__img_hdr_crc = 0);
PROVIDE(__img_hdr_checked = 0);

PROVIDE(_heap_start = _ebss);
PROVIDE(_heap_end = _estack);
