#pragma once

#if defined(DEBUG)
  #include <SEGGER_RTT.h>
  #define debug(msg, ...)  SEGGER_RTT_printf(0, msg, ##__VA_ARGS__)
  #define debugln(msg, ...)  debug(msg "\n", ##__VA_ARGS__)
#else
  #define debug(...)
  #define debugln(...)
#endif
