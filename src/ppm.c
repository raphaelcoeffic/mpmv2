#include <driverlib/ioc.h>
#include <driverlib/timer.h>
#include <driverlib/prcm.h>
#include <stdint.h>

#include "ppm.h"
#include "timer.h"

//
// PPM / CCP port mapping:
// 
// GPT 0A: IOC_PORT_MCU_PORT_EVENT0
// GPT 0B: IOC_PORT_MCU_PORT_EVENT1
// GPT 1A: IOC_PORT_MCU_PORT_EVENT2
// GPT 1B: IOC_PORT_MCU_PORT_EVENT3
// GPT 2A: IOC_PORT_MCU_PORT_EVENT4
// GPT 2B: IOC_PORT_MCU_PORT_EVENT5
// GPT 3A: IOC_PORT_MCU_PORT_EVENT6
// GPT 3B: IOC_PORT_MCU_PORT_EVENT7

#define TIMER_BASE     GPT2_BASE
#define TIMER_PERIPH   PRCM_PERIPH_TIMER2
#define TIMER_INST     TIMER_A
#define TIMER_IOC_PORT IOC_PORT_MCU_PORT_EVENT4
#define TIMER_CONFIG   (TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_CAP_TIME_UP)
#define TIMER_EVENT    TIMER_CAPA_EVENT


static uint16_t ppm_channels[PPM_MAX_CHANNELS];
static volatile uint16_t valid_channels;
static volatile uint32_t edge_start;


static inline bool edge_detected() {
  return TimerIntStatus(TIMER_BASE, false) & TIMER_EVENT;
}

static inline void clear_edge_bit() {
  TimerIntClear(TIMER_BASE, TIMER_EVENT);
}

static inline uint32_t match_register() {
  return HWREG(TIMER_BASE + GPT_O_TAR);
}

static void _edge_time_isr()
{
  uint32_t last_edge = match_register();
  clear_edge_bit();

  uint32_t pulse = (last_edge - edge_start) / 48;
  if (pulse < PULSE_MIN_US || pulse > PULSE_MAX_US) {
    valid_channels = 0;   
  } else {
    ppm_channels[valid_channels++] = (uint16_t)pulse;
  }

  edge_start = last_edge;
}

void ppm_timer_init(uint32_t pin)
{
  PRCMPeripheralRunEnable(TIMER_PERIPH);
  PRCMLoadSet();

  TimerDisable(TIMER_BASE, TIMER_INST);
  IOCPortConfigureSet(pin, TIMER_IOC_PORT, IOC_STD_INPUT);

  // use GPT2 A in edge-time  / count-up mode
  TimerConfigure(TIMER_BASE, TIMER_CONFIG);
  TimerEventControl(TIMER_BASE, TIMER_INST, TIMER_EVENT_NEG_EDGE);

  // prescaler must be set so that we can use the 24-bit extention
  // (does not work as prescaler though)
  TimerPrescaleSet(TIMER_BASE, TIMER_INST, 0xFFFF);

  // stop timer in debugger
  TimerStallControl(TIMER_BASE, TIMER_INST, true);

  // register capture IRQ
  TimerIntDisable(TIMER_BASE, TIMER_EVENT);
  TimerIntRegister(TIMER_BASE, TIMER_INST, _edge_time_isr);
}

void ppm_timer_start()
{
  valid_channels = edge_start = 0;
  TimerLoadSet(TIMER_BASE, TIMER_INST, 0);

  TimerIntEnable(TIMER_BASE, TIMER_EVENT);
  TimerEnable(TIMER_BASE, TIMER_INST);
}

void ppm_timer_stop()
{
  TimerIntDisable(TIMER_BASE, TIMER_EVENT);
  TimerDisable(TIMER_BASE, TIMER_INST);
}

bool detect_ppm(uint32_t timeout_ms)
{
  ppm_timer_start();

  uint32_t timeout = millis() + timeout_ms;
  while (millis_before(timeout)) {
    if (valid_channels >= PPM_MIN_CHANNELS) break;
  }
  ppm_timer_stop();

  return valid_channels >= PPM_MIN_CHANNELS ? true : false;
}

const uint16_t* ppm_get_channels()
{
  return ppm_channels;
}

uint32_t ppm_get_valid_channels()
{
  return valid_channels;
}
