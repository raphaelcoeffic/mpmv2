#include <driverlib/event.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/timer.h>

#include <stdint.h>

#include "dma.h"
#include "ppm.h"
#include "timer.h"

//
// PPM / CCP port mapping:
// =======================
// 
//   GPT 0A: IOC_PORT_MCU_PORT_EVENT0
//   GPT 0B: IOC_PORT_MCU_PORT_EVENT1
//   GPT 1A: IOC_PORT_MCU_PORT_EVENT2
//   GPT 1B: IOC_PORT_MCU_PORT_EVENT3
//   GPT 2A: IOC_PORT_MCU_PORT_EVENT4
//   GPT 2B: IOC_PORT_MCU_PORT_EVENT5
//   GPT 3A: IOC_PORT_MCU_PORT_EVENT6
//   GPT 3B: IOC_PORT_MCU_PORT_EVENT7
// 
// DMA channel mapping:
// ====================
// 
//   UDMA_CHAN_TIMER0_A -> Timer0 A event
//   UDMA_CHAN_TIMER0_B -> Timer0 B event
//   UDMA_CHAN_TIMER1_A -> Timer1 A event
//   UDMA_CHAN_TIMER1_B -> Timer1 B event
//
// 45h = Not used tied to 0
// 4Dh = GPT0A DMA trigger event
// 4Eh = GPT0B DMA trigger event
// 4Fh = GPT1A DMA trigger event
// 50h = GPT1B DMA trigger event
// 51h = GPT2A DMA trigger event
// 52h = GPT2B DMA trigger event
// 53h = GPT3A DMA trigger event
// 54h = GPT3B DMA trigger event

#define TIMER_BASE     GPT0_BASE
#define TIMER_PERIPH   PRCM_PERIPH_TIMER0
#define TIMER_IOC_PORT IOC_PORT_MCU_PORT_EVENT0
#define TIMER_INST     TIMER_A

#define TIMER_CONFIG   (TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_CAP_TIME_UP)
#define TIMER_EVENT    TIMER_CAPA_EVENT

#define TIMER_DMA_EV    GPT_DMAEV_CAEDMAEN
#define TIMER_DMA_CHAN  UDMA_CHAN_TIMER0_A
#define DMA_BUFFER_SIZE PPM_MAX_CHANNELS

static uint16_t ppm_channels[PPM_MAX_CHANNELS];
static volatile uint16_t valid_channels;
static volatile uint32_t edge_start;

typedef enum {
  PPM_MODE_NORMAL = 0,
  PPM_MODE_DETECT,
} ppm_timer_mode_t;

static ppm_timer_mode_t ppm_timer_mode = PPM_MODE_NORMAL;

// used only for detection for now
static uint32_t dma_buffer[DMA_BUFFER_SIZE];

static inline bool capture_event_detected() {
  return TimerIntStatus(TIMER_BASE, true) & TIMER_EVENT;
}

static inline void clear_capture_event() {
  TimerIntClear(TIMER_BASE, TIMER_EVENT);
}

static inline uint32_t match_register() {
  return HWREG(TIMER_BASE + GPT_O_TAR);
}

static inline void enable_dma_channel(uint32_t channel) {
  HWREG(UDMA0_BASE + UDMA_O_SETCHANNELEN) = 1 << channel;
}

static inline void disable_dma_channel(uint32_t channel) {
  HWREG(UDMA0_BASE + UDMA_O_CLEARCHANNELEN) = 1 << channel;
}

static inline void enable_dma_capture_event() {
  HWREG(TIMER_BASE + GPT_O_DMAEV) |= TIMER_DMA_EV;
}

static inline void disable_dma_capture_event() {
  HWREG(TIMER_BASE + GPT_O_DMAEV) &= ~TIMER_DMA_EV;
}

static inline bool timer_dma_done() {
  return TimerIntStatus(TIMER_BASE, false) & TIMER_TIMA_DMA;
}

static inline void clear_timer_dma_done() {
  TimerIntClear(TIMER_BASE, TIMER_TIMA_DMA);
}

static inline void setup_dma_xfer(uint32_t* buffer, uint32_t len)
{
  // configure event fabric: GPT0A DMA REQ -> uDMA channel 9
  EventRegister(EVENT_O_UDMACH9SSEL, EVENT_UDMACH9SSEL_EV_GPT0A_DMABREQ);

  tDMAControlTable *dma_entry = dma_channel_entry(TIMER_DMA_CHAN);
  dma_entry->pvSrcEndAddr = (void *)(TIMER_BASE + GPT_O_TAR);
  dma_entry->pvDstEndAddr = (void *)(buffer + len - 1);

  uint32_t ctrl = UDMA_MODE_BASIC | UDMA_SIZE_32 | UDMA_SRC_INC_NONE |
                  UDMA_DST_INC_32 | UDMA_ARB_1;

  ctrl |= (len - 1) << UDMA_XFER_SIZE_S;
  dma_entry->ui32Control = ctrl;
}

static void _start_rx_dma_xfer(uint32_t* buffer, uint32_t len)
{
  setup_dma_xfer(buffer, len);
  enable_dma_channel(TIMER_DMA_CHAN);
  enable_dma_capture_event();

  TimerIntEnable(TIMER_BASE, TIMER_TIMA_DMA);
  TimerEnable(TIMER_BASE, TIMER_INST);
}

static void _stop_rx_dma()
{
  disable_dma_channel(TIMER_DMA_CHAN);
  disable_dma_capture_event();

  TimerIntDisable(TIMER_BASE, TIMER_TIMA_DMA);
  TimerDisable(TIMER_BASE, TIMER_INST);
}

static void _ppm_timer_isr()
{
  if (capture_event_detected()) {
    uint32_t last_edge = match_register();
    clear_capture_event();

    if (ppm_timer_mode == PPM_MODE_NORMAL) {
      uint32_t pulse = (last_edge - edge_start) / 48;
      if (pulse < PULSE_MIN_US || pulse > PULSE_MAX_US) {
        valid_channels = 0;   
      } else if (valid_channels < PPM_MAX_CHANNELS){
        ppm_channels[valid_channels++] = (uint16_t)pulse;
      }
      edge_start = last_edge;
      return;
    }

    if (ppm_timer_mode == PPM_MODE_DETECT) {
      uint32_t pulse = (last_edge - edge_start) / 48;
      edge_start = last_edge;

      if (pulse > PULSE_MAX_US) {
        _start_rx_dma_xfer(dma_buffer, DMA_BUFFER_SIZE);
        TimerIntDisable(TIMER_BASE, TIMER_EVENT);
      }
      return;
    }
  }

  if (timer_dma_done()) {
    clear_timer_dma_done();

    TimerIntDisable(TIMER_BASE, TIMER_TIMA_DMA);
    TimerDisable(TIMER_BASE, TIMER_INST);

    // TODO: what else? some flag? callback?
  }
}

void ppm_timer_init(uint32_t pin)
{
  PRCMPeripheralRunEnable(TIMER_PERIPH);
  PRCMLoadSet();
  dma_init();

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
  TimerIntRegister(TIMER_BASE, TIMER_INST, _ppm_timer_isr);
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

void ppm_start_detect()
{
  ppm_timer_mode = PPM_MODE_DETECT;
  ppm_timer_start();
}

void ppm_stop_detect()
{
  _stop_rx_dma();
  ppm_timer_mode = PPM_MODE_NORMAL;

  // compute timings
  ppm_channels[0] = (dma_buffer[0] - edge_start) / 48;
  for (int i = 1; i < PPM_MAX_CHANNELS; i++) {
    ppm_channels[i] = (dma_buffer[i] - dma_buffer[i - 1]) / 48;
  }
}

bool ppm_detect_done()
{
  // we're done when the timer has been disabled
  return HWREG(TIMER_BASE + GPT_O_CTL) & GPT_CTL_TAEN ? false : true; 
}

const uint16_t* ppm_get_channels()
{
  return ppm_channels;
}

uint32_t ppm_get_valid_channels()
{
  return valid_channels;
}
