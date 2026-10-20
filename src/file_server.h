#pragma once

#include <stdint.h>

#include <lfs.h>

// Files of the file system read and written from a host (tools/mpm_fs.py),
// one request at a time. The transport (mpm_port.c: frames starting with
// "FS" on the USB link, replies in MPM_TELEM_FILE telemetry) carries:
//
//   request:  seq, op, payload, CRC-32 (zlib's, little endian, of seq to the
//             end of the payload)
//   reply:    seq, op, status (0 or a negative LFS_ERR_*), payload, CRC-32
//
// A request with the seq and op of the last one gets the same reply again
// without running again (the host retries when a reply is lost).
//
// Payloads (integers little endian, paths not terminated, up to
// FILE_SERVER_PATH_MAX bytes):
//
//   INFO    -                           -> block size u32, block count u32,
//                                          blocks used u32
//   LIST    index u16, dir              -> more u8, entries from index:
//                                          type u8 (1 file, 2 dir), size u32,
//                                          name length u8, name
//   STAT    path                        -> type u8, size u32
//   READ    offset u32, length u8, path -> data (shorter at the end)
//   OPEN    size u32, CRC-32 u32, path  -> -   upload: a new content for path
//   WRITE   offset u32, data            -> -   in order from 0
//   CLOSE   -                           -> -   size and CRC-32 checked, then
//                                              path replaced at once; else
//                                              LFS_ERR_CORRUPT, path as it was
//   REMOVE  path                        -> -
//   RENAME  old path, 0, new path       -> -
//   MKDIR   path                        -> -
//
// The upload goes to FILE_SERVER_UPLOAD until CLOSE: a power cut or an
// abandoned upload leaves the old file.

#define FILE_SERVER_INFO   0
#define FILE_SERVER_LIST   1
#define FILE_SERVER_STAT   2
#define FILE_SERVER_READ   3
#define FILE_SERVER_OPEN   4
#define FILE_SERVER_WRITE  5
#define FILE_SERVER_CLOSE  6
#define FILE_SERVER_REMOVE 7
#define FILE_SERVER_RENAME 8
#define FILE_SERVER_MKDIR  9

#define FILE_SERVER_PATH_MAX   96
#define FILE_SERVER_DATA_MAX   128  // READ and WRITE
#define FILE_SERVER_REQ_MAX    (2 + 4 + FILE_SERVER_DATA_MAX + 4)
#define FILE_SERVER_REPLY_MAX  250
#define FILE_SERVER_UPLOAD     ".upload"

// lfs: a mounted file system, or NULL (every request: LFS_ERR_IO)
void file_server_init(lfs_t* lfs);

// Before the file system is written elsewhere (an EEPROM flush): closes the
// file READ keeps open, which would go on reading a replaced file's old
// blocks
void file_server_sync();

// One request: the reply into reply (FILE_SERVER_REPLY_MAX bytes), its
// length; 0 for a request too short or with a bad CRC (no reply)
uint8_t file_server_request(const uint8_t* req, uint8_t len, uint8_t* reply);
