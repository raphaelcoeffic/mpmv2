// What TI's LRF layer (lib/ti/drivers/rcl) needs from the RCL HAL
// (RCLLPF3_Hal.c and RCL_Feature.c of the SDK) on bare metal: clocks, time,
// DC/DC peak current, the HFXT ratio. No temperature or supply monitoring
// yet: 25 C and 3.3 V for the trims and the TX power compensation.

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_ckmd.h)
#include DeviceFamily_constructPath(inc/hw_sys0.h)
#include DeviceFamily_constructPath(inc/hw_systim.h)
#include DeviceFamily_constructPath(driverlib/lrfd.h)
#include <ti/drivers/dpl/HwiP.h>
#include <ti/drivers/rcl/RCL_Feature.h>
#include <ti/drivers/rcl/RCL_Scheduler.h>
#include <ti/drivers/rcl/hal/RCL_Hal.h>

// TI's default turns on temperature monitoring (through the RCL scheduler)
const RCL_FeatureControl rclFeatureControl = {
    .enableTemperatureMonitoring = false,
    .enablePaEsdProtection = false,
    .enableTxOutputPowerCompensation = false,
};

// Only used by the temperature notification path
RCL_SchedulerState rclSchedulerState;

bool RCL_Scheduler_postEvent(RCL_Command_Handle c, RCL_Events e)
{
  (void)c;
  (void)e;
  return false;
}

void RCL_Hal_setTemperatureNotification(int16_t temperature, uint16_t threshold,
                                        void (*callback)(int16_t))
{
  (void)temperature;
  (void)threshold;
  (void)callback;
}

void RCL_Hal_stopTemperatureNotification(void) {}
void RCL_Hal_powerSetSwTcxoUpdateConstraint(void) {}
void RCL_Hal_powerReleaseSwTcxoUpdateConstraint(void) {}

int16_t RCL_Hal_getTemperature(void) { return 25; }

uint16_t RCL_Hal_getVddsVoltage(void) { return 3300; }

uint32_t RCL_Hal_getCurrentTime(void) { return HWREG(SYSTIM_BASE + SYSTIM_O_TIME250N); }

uint32_t RCL_Hal_getHfxtRatio(void)
{
  return (HWREG(CKMD_BASE + CKMD_O_HFTRACKCTL) & CKMD_HFTRACKCTL_RATIO_M) >> CKMD_HFTRACKCTL_RATIO_S;
}

// As TI's (the reset ratio, 0x400000, scales by 1 anyway)
uint32_t RCL_Hal_getHfxtRatioDefault(void) { return 0x00200000U; }

void RCL_Hal_setRclClockEnable(uint16_t mask)
{
  uintptr_t key = HwiP_disable();
  LRFDSetClockDependency(mask, LRFD_CLK_DEP_RCL);
  HwiP_restore(key);
}

void RCL_Hal_clearRclClockEnable(uint16_t mask)
{
  uintptr_t key = HwiP_disable();
  LRFDReleaseClockDependency(mask, LRFD_CLK_DEP_RCL);
  HwiP_restore(key);
}

uint8_t RCL_Hal_setDcdcIpeakSetting(uint8_t setting)
{
  uint8_t previous = (HWREG(SYS0_BASE + SYS0_O_TMUTE4) & SYS0_TMUTE4_IPEAK_M) >> SYS0_TMUTE4_IPEAK_S;
  uintptr_t key = HwiP_disable();
  // Mutable registers: written within 32 cycles of the unlock
  HWREG(SYS0_BASE + SYS0_O_MUNLOCK) = SYS0_MUNLOCK_KEY_UNLOCK;
  HWREG(SYS0_BASE + SYS0_O_TMUTE4) = (HWREG(SYS0_BASE + SYS0_O_TMUTE4) & ~SYS0_TMUTE4_IPEAK_M) |
                                     (((uint32_t)setting << SYS0_TMUTE4_IPEAK_S) & SYS0_TMUTE4_IPEAK_M);
  HWREG(SYS0_BASE + SYS0_O_MUNLOCK) = SYS0_MUNLOCK_KEY_LOCK;
  HwiP_restore(key);
  return previous;
}

void RCL_Hal_enableClkBuffer(void) { HWREG(CKMD_BASE + CKMD_O_HFXTCTL) |= CKMD_HFXTCTL_HPBUFEN; }
