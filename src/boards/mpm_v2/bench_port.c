// The radio bench (bench_port.h) on the MPM v2 and the CC2652 LaunchPad.
// RF activity LED: the MPM v2's RGB LED, TX orange, RX green, RX with a bad
// CRC red, idle dim blue (as at boot); the LaunchPad's, TX red, RX green, bad
// CRC both. Timing edges: the MPM v2's PA/LNA CTX (the PA enable, no LNA
// enable pin), the LaunchPad's LEDs (red the PA enable, green the LNA's)
#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>

#include "bench_port.h"
#include "board.h"
#include "timer.h"

#if defined(LED_SPI) && defined(LED_DIN)
  #include "led_rgb.h"
#endif

const char bench_port_dev[] = "cc2652";

bool bench_port_irq_disable() { return IntMasterDisable(); }

void bench_port_irq_enable() { IntMasterEnable(); }

static bench_led_t _led;  // shown, again after the timing pins

void bench_port_led(bench_led_t state)
{
  _led = state;
#if defined(LED_SPI) && defined(LED_DIN)
  static const uint32_t colors[] = {
      [BENCH_LED_IDLE] = LED_COLOR(0x00, 0x00, 0x60),
      [BENCH_LED_TX] = LED_COLOR(0x60, 0x20, 0x00),
      [BENCH_LED_RX] = LED_COLOR(0x00, 0x60, 0x00),
      [BENCH_LED_RX_BAD] = LED_COLOR(0x60, 0x00, 0x00),
  };
  led_rgb_set_color(colors[state]);
#elif defined(LED_GREEN) && defined(LED_RED)
  GPIO_writeDio(LED_RED, state == BENCH_LED_TX || state == BENCH_LED_RX_BAD);
  GPIO_writeDio(LED_GREEN, state == BENCH_LED_RX || state == BENCH_LED_RX_BAD);
#endif
}

// The RF core's PA and LNA enable outputs (RFC_GPO1, RFC_GPO0), edges in
// the GPIO edge interrupt
#if defined(PA_CTX)
  #define TIMING_PA PA_CTX  // the PA/LNA's CTX (board.c), no LNA enable pin
#elif defined(LED_RED) && defined(LED_GREEN)
  #define TIMING_PA LED_RED
  #define TIMING_LNA LED_GREEN
#endif

#if defined(TIMING_PA)
static void _edge_pin(uint32_t pin, uint8_t signal, uint32_t ticks)
{
  if (!GPIO_getEventDio(pin)) return;
  GPIO_clearEventDio(pin);
  bench_edge(ticks, signal, GPIO_readDio(pin));
}

static void _edge_isr()
{
  uint32_t t = get_ticks();
  _edge_pin(TIMING_PA, BENCH_EDGE_PA, t);
  #if defined(TIMING_LNA)
  _edge_pin(TIMING_LNA, BENCH_EDGE_LNA, t);
  #endif
}
#endif

// On: the RF core outputs, input buffer and edge interrupt on. Off: the
// pins as before
bool bench_port_timing(bool on)
{
#if defined(TIMING_PA)
  static bool isr;
  if (!isr) {
    isr = true;
    IntRegister(INT_AON_GPIO_EDGE, _edge_isr);
  }
  uint32_t capture = IOC_STD_OUTPUT | IOC_INPUT_ENABLE | IOC_BOTH_EDGES | IOC_INT_ENABLE;
  if (on) {
    IOCPortConfigureSet(TIMING_PA, IOC_PORT_RFC_GPO1, capture);
  #if defined(TIMING_LNA)
    IOCPortConfigureSet(TIMING_LNA, IOC_PORT_RFC_GPO0, capture);
  #endif
    GPIO_clearEventDio(TIMING_PA);
    IntPendClear(INT_AON_GPIO_EDGE);
    IntEnable(INT_AON_GPIO_EDGE);
    return true;
  }
  IntDisable(INT_AON_GPIO_EDGE);
  #if defined(PA_CTX)
  IOCPortConfigureSet(PA_CTX, IOC_PORT_RFC_GPO1, IOC_STD_OUTPUT);
  #else
  IOCPinTypeGpioOutput(LED_RED);
  IOCPinTypeGpioOutput(LED_GREEN);
  bench_port_led(_led);
  #endif
  return true;
#else
  (void)on;
  return false;
#endif
}

bool bench_port_timing_lna()
{
#if defined(TIMING_LNA)
  return true;
#else
  return false;
#endif
}
