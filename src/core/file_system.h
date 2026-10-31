#pragma once

#include <lfs.h>

#define KILOBYTE 1024
#define MEGABYTE (1024 * 1024)

// littlefs caches: a flash page. Also the size of a file's buffer
// (lfs_file_opencfg(): no malloc)
#define FILE_SYSTEM_CACHE_SIZE 256

// littlefs on the whole external NOR flash (MX25R8035F, 1 MB). Formatted if
// there is no file system, grown to the flash size if smaller. 0 or a
// littlefs error
int file_system_init(lfs_t* lfs);
