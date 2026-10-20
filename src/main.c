#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/uart.h>

#include "bench.h"
#include "board.h"
#include "file_system.h"
#include "mpm/mpm.h"
#include "serial.h"
#include "shell.h"
#include "timer.h"
#include "uart.h"

#if defined(LED_SPI) && defined(LED_DIN)
  #include "led_rgb.h"
#endif

#include "debug.h"

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

int main(void)
{
  board_init();
  timer_init();
  leds_init();

  debugln("## Boot completed ##");

#if defined(BENCH)
  bench_loop();
#endif

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

  mpm_init();
  mpm_run();
}
