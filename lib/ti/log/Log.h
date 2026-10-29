/* Stand-in for TI's ti/log/Log.h (SimpleLink SDK logging, not used here):
   the LRF layer's log calls compile to nothing */
#ifndef ti_log_Log__include
#define ti_log_Log__include

#define Log_printf(...) do {} while (0)
#define Log_DEBUG 0
#define Log_VERBOSE 0
#define Log_INFO 0
#define Log_WARNING 0
#define Log_ERROR 0

#endif
