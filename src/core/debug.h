#pragma once

// Debug lines: into the log (log.h: kept, to USB when enabled, for users
// without a debugger), and with -DDEBUG=ON to Segger RTT as well
#include "log.h"

#if defined(DEBUG)
  #include <SEGGER_RTT.h>
  #define debug(msg, ...)                          \
    do {                                           \
      SEGGER_RTT_printf(0, msg, ##__VA_ARGS__);    \
      log_printf(msg, ##__VA_ARGS__);              \
    } while (0)
#else
  #define debug(msg, ...) log_printf(msg, ##__VA_ARGS__)
#endif
#define debugln(msg, ...) debug(msg "\n", ##__VA_ARGS__)
