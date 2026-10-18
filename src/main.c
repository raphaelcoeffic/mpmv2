#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/uart.h>

#include "board.h"
#include "file_system.h"
#include "ppm.h"
#include "serial.h"
#include "shell.h"
#include "timer.h"
#include "uart.h"

#if defined(LED_SPI) && defined(LED_DIN)
  #include "led_rgb.h"
#endif

#include "debug.h"

#define MPM_BAUDRATE   100000
#define MPM_SER_MODE   UART_8E2

// File system variables
lfs_t lfs;
lfs_file_t file;

static bool detect_button()
{
  uint32_t pin = BUTTON;
  IOCPinTypeGpioInput(pin);
  IOCIOPortPullSet(pin, IOC_IOPULL_UP);
  delay_us(100);

  uint32_t start = millis();
  while ((millis() - start) < 10 && !GPIO_readDio(pin)) {}

  return millis() - start >= 10 ? true : false;
}

static void leds_init()
{
#if defined(LED_GREEN) && defined(LED_RED)
  IOCPinTypeGpioOutput(LED_GREEN);
  IOCPinTypeGpioOutput(LED_RED);
  GPIO_clearDio(LED_GREEN);
  GPIO_clearDio(LED_RED);
#endif

#if defined(LED_SPI) && defined(LED_DIN)
  led_rgb_init(LED_SPI, LED_DIN);
  led_rgb_set_color(LED_COLOR(0x00, 0x00, 0x60));
#endif
}

static volatile bool ppm_frame_received = false;

static void ppm_frame_timeout(const uint16_t* ppm, uint32_t channels)
{
  (void)ppm;
  (void)channels;
  ppm_frame_received = true;
}

static const char lorem_ipsum[] =
    // Section 1.10.32 of "de Finibus Bonorum et Malorum", written by Cicero in 45 BC
    "Sed ut perspiciatis unde omnis iste natus error sit voluptatem \n"
    "accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab\n"
    "illo inventore veritatis et quasi architecto beatae vitae dicta sunt\n"
    "explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut\n"
    "odit aut fugit, sed quia consequuntur magni dolores eos qui ratione\n"
    "voluptatem sequi nesciunt. Neque porro quisquam est, qui dolorem ipsum\n"
    "quia dolor sit amet, consectetur, adipisci velit, sed quia non numquam\n"
    "eius modi tempora incidunt ut labore et dolore magnam aliquam quaerat\n"
    "voluptatem. Ut enim ad minima veniam, quis nostrum exercitationem ullam\n"
    "corporis suscipit laboriosam, nisi ut aliquid ex ea commodi consequatur?\n"
    "Quis autem vel eum iure reprehenderit qui in ea voluptate velit esse quam\n"
    "nihil molestiae consequatur, vel illum qui dolorem eum fugiat quo \n"
    "voluptas nulla pariatur?\n";

static void test_print_file()
{
  int err = lfs_file_open(&lfs, &file, "lorem_ipsum", LFS_O_RDWR | LFS_O_CREAT);
  if (err) return;

  uint32_t size = lfs_file_size(&lfs, &file);
  if (size == 0) {
    int written = lfs_file_write(&lfs, &file, lorem_ipsum, sizeof(lorem_ipsum) - 1);
    if (written > 0) size = written;
    lfs_file_rewind(&lfs, &file);
    debugln("file created");
  } else {
    debugln("file size = %d", size);
  }

  if (size > 0) {
    char buffer[32];
    int read_count = 0;
    while (read_count < size) {
      int read = lfs_file_read(&lfs, &file, buffer, sizeof(buffer));
      if (read > 0) {
        read_count += read;
        serial_write_dma((uint8_t*)buffer, read, true);
      }
    }
  }

  lfs_file_close(&lfs, &file);
}

typedef enum {
  SIGNAL_TYPE_NONE = 0,
  SIGNAL_TYPE_PPM,
  SIGNAL_TYPE_SERIAL,  
} signal_type_t;

static signal_type_t analyse_input_signal()
{
  ppm_stop_detect();

  uint32_t max_channels = ppm_get_valid_channels();
  const uint16_t* ppm_channels = ppm_get_channels();

  int valid_ppm_channels = 0;
  for (int i = 0; i < max_channels; i++) {
    if (ppm_channels[i] < PULSE_MIN_US || ppm_channels[i] > PULSE_MAX_US) {
      break;
    }
    valid_ppm_channels++;
  }

  if (valid_ppm_channels >= PPM_MIN_CHANNELS) {
    return SIGNAL_TYPE_PPM;
  }

  int long_pulses = 0;
  for (int i = 0; i < max_channels; i++) {
    if (ppm_channels[i] > 200) long_pulses++;
  }

  return long_pulses <= 1 ? SIGNAL_TYPE_SERIAL : SIGNAL_TYPE_NONE;
}

static signal_type_t detect_input_signal()
{
  ppm_timer_init(SERIAL_RX_IOD);
  ppm_start_detect();

  uint32_t timeout = millis() + 100;
  while (!ppm_detect_done() && millis_before(timeout)) {}

  if (millis_after(timeout)) {
    return SIGNAL_TYPE_NONE;
  }

  return analyse_input_signal();
}

static void init_signal_receiver(signal_type_t t)
{
  switch (t) {
    case SIGNAL_TYPE_PPM:
      debugln("PPM detected");
      ppm_timer_start();
      ppm_timer_set_timeout(2400, ppm_frame_timeout);
      break;

    case SIGNAL_TYPE_SERIAL:
      debugln("SERIAL detected");
      serial_init(MPM_BAUDRATE, MPM_SER_MODE);
      break;

    default:
      debugln("no signal detected");
      break;
  }  
}

static void stop_signal_receiver(signal_type_t t)
{
  switch (t) {
    case SIGNAL_TYPE_PPM:
      ppm_timer_set_timeout(0, 0);
      ppm_timer_stop();
      ppm_frame_received = false;
      break;

    case SIGNAL_TYPE_SERIAL:
      serial_stop();
      break;

    default:
      break;
  }
}

static void main_loop()
{
  signal_type_t signal = SIGNAL_TYPE_NONE;
  uint32_t timeout = 0;
  bool detect = false;

  while (true) {
    bool frame_received = false;

    switch (signal) {
    case SIGNAL_TYPE_NONE:
      if (!detect) {
        detect = true;
        ppm_timer_init(SERIAL_RX_IOD);
        ppm_start_detect();
        continue;
      }
      if (ppm_detect_done()) {
        detect = false;
        signal = analyse_input_signal();
        frame_received = (signal != SIGNAL_TYPE_NONE);
        init_signal_receiver(signal);
      }
      break;

    case SIGNAL_TYPE_PPM:
      if (ppm_frame_received) {
        ppm_frame_received = false;
        frame_received = true;
      }
      break;

    case SIGNAL_TYPE_SERIAL:
      if (serial_frame_received) {
        serial_frame_received = false;
        if (rx_len > 0 && rx_buf[0] == 0x55) {
          frame_received = true;
        }
        uart_reset_rx_len(UART0);
      }
      break;
    }

    if (frame_received) {
      timeout = millis() + 50;
    } else if (signal != SIGNAL_TYPE_NONE && millis_after(timeout)) {
      debugln("signal timeout");
      stop_signal_receiver(signal);
      signal = SIGNAL_TYPE_NONE;
    }
  }
}

int main(void)
{
  board_init();
  timer_init();
  leds_init();

// #if defined(TEST_PIN)
  // GPIO_setDio(TEST_PIN);
  // IOCPinTypeGpioOutput(TEST_PIN);
// #endif

  debugln("## Boot completed ##");

  bool button_pressed = detect_button();
  if (button_pressed) {
    debugln("button pressed detected");
    shell_loop();
  }

  // int err = file_system_init(&lfs);
  // if (err != 0) {
  //   debugln("error: file system init failed");
  // } else {
  //   debugln("file system mounted");
  //   // test_print_file();
  // }

  main_loop();
}
