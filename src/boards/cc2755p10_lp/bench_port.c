// The radio bench (bench_port.h) on the CC2755P10 LaunchPad. RF activity
// LED: TX red, RX green, bad CRC both. Timing: no edge capture; the RFE's PA
// and LNA enables (TI's RFE images drive them on RFE GPO0 and GPO1) go out
// on LRFD0 / LRFD1, DIO17 / DIO18 (BoosterPack header pins 32 / 31, behind
// solder bridges) for a logic analyzer, each run's start a 1 us pulse on
// DIO17
#include <driverlib/gpio.h>
#include <inc/hw_lrfddbell.h>
#include <ti/drivers/dpl/HwiP.h>

#include "bench_port.h"
#include "board.h"
#include "timer.h"

const char bench_port_dev[] = "cc2755";

bool bench_port_irq_disable() { return HwiP_disable() != 0; }

void bench_port_irq_enable() { HwiP_restore(0); }

void bench_port_led(bench_led_t state)
{
  board_led(BOARD_LED_RED, state == BENCH_LED_TX || state == BENCH_LED_RX_BAD);
  board_led(BOARD_LED_GREEN, state == BENCH_LED_RX || state == BENCH_LED_RX_BAD);
}

#define TIMING_PA 17   // LRFD0
#define TIMING_LNA 18  // LRFD1
#define TIMING_IOC_LRFD 5
// GPOSEL sources: RFE GPO line 0 (PA enable), line 1 (LNA enable)
#define GPOSEL_RFEGPO0 0x11
#define GPOSEL_RFEGPO1 0x12

// On: the RFE's GPOs out on the pins. Off: the pins back to GPIO, low. The
// edges are for the logic analyzer: the bench's own report says missed
bool bench_port_timing(bool on)
{
  if (on) {
    HWREG(LRFDDBELL_BASE + LRFDDBELL_O_GPOSEL0) =
        GPOSEL_RFEGPO0 << LRFDDBELL_GPOSEL0_SRC0_S | GPOSEL_RFEGPO1 << LRFDDBELL_GPOSEL0_SRC1_S;
    GPIOClearDio(TIMING_PA);
    GPIOSetOutputEnableDio(TIMING_PA, GPIO_OUTPUT_ENABLE);
    board_ioc(TIMING_PA, TIMING_IOC_LRFD);
    board_ioc(TIMING_LNA, TIMING_IOC_LRFD);
    return true;
  }
  board_ioc(TIMING_PA, IOC_IOC0_PORTCFG_BASE);
  board_ioc(TIMING_LNA, IOC_IOC0_PORTCFG_BASE);
  HWREG(LRFDDBELL_BASE + LRFDDBELL_O_GPOSEL0) = 0;
  return true;
}

bool bench_port_timing_lna() { return true; }

// DIO17 high for about 1 us as a GPIO, then the PA enable again (off at
// every run's start)
void bench_port_timing_mark()
{
  GPIOSetDio(TIMING_PA);
  board_ioc(TIMING_PA, IOC_IOC0_PORTCFG_BASE);
  delay_us(1);
  board_ioc(TIMING_PA, TIMING_IOC_LRFD);
  GPIOClearDio(TIMING_PA);
}
