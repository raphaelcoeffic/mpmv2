#pragma once

#include <lfs.h>

// Settings files (all optional: without them, the build's defaults):
//
//   ppm.txt   the PPM mode's model (mpm_settings.h: mpm_ppm_parse()). Bad
//             lines are listed in ppm.err (removed when there are none) and
//             keep their default
//   debug.txt "usb = on": the debug log (log.h) on the USB link from power
//             up (tools/mpm_fs.py log); without it, only while a host asks.
//             "antenna = 2": RF on J3 (default 1, J2). Errors in debug.err
//
// Read at power up and when the file server replaces, removes or renames
// one (mpm_fs.py put): applied at once
#define SETTINGS_PPM       "ppm.txt"
#define SETTINGS_PPM_ERR   "ppm.err"
#define SETTINGS_DEBUG     "debug.txt"
#define SETTINGS_DEBUG_ERR "debug.err"
#define SETTINGS_MAX       1024  // bytes of a settings file read

// lfs: a mounted file system, or NULL (the defaults)
void settings_init(lfs_t* lfs);

// A file changed (file server): reads it again if it is a settings file
void settings_file_changed(const char* path);
