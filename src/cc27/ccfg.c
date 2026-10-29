// CCFG of the CC27xx applications: TI's defaults (no flash protection, not
// even the CCFG's own sector, debug open, chip erase allowed), the
// application's vector table at 0, and as the LaunchPad's stock CCFG the ROM
// serial boot loader as the FCFG sets it and a chip erase that can keep the
// HSM firmware (sectors 464 to 511, 0xE8000 to the end). The CRCs
// (0x0BAD0BAD here) are filled in after the link (tools/cc27_ccfg.py).

#include <inc/hw_types.h>
#include <inc/hw_device.h>
#include <inc/hw_ccfg.h>

extern void (*const resetVectors[])(void);

__attribute__((section(".ccfg"), used)) const ccfg_t ccfg = {
    CCFG_DEFAULT_VALUES,
    .bootCfg.pBldrVtor = CCFG_BC_PBLDR_USE_FCFG,
    .bootCfg.pAppVtor = (void *)resetVectors,
    .flashProt.chipEraseRetain.mainSectors256_511 = 0xFC000000,
};
