/* Stand-in for TI's ti/drivers/dpl/HwiP.h (driver porting layer): the
   critical sections of the LRF layer, on PRIMASK */
#ifndef ti_drivers_dpl_HwiP__include
#define ti_drivers_dpl_HwiP__include

#include <stdint.h>

static inline uintptr_t HwiP_disable(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(primask)::"memory");
    return primask;
}

static inline void HwiP_restore(uintptr_t key)
{
    __asm volatile("msr primask, %0" ::"r"((uint32_t)key) : "memory");
}

#endif
