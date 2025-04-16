#pragma once

#include <lfs.h>

#define KILOBYTE 1024
#define MEGABYTE (1024 * 1024)

// File system in the last 128KB of FLASH
#define FS_SIZE     (256 * KILOBYTE)
#define FS_OFFSET   0

int file_system_init(lfs_t* lfs);
