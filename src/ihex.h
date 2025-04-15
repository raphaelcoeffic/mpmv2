#pragma once

typedef void (*ihex_flush_cb_t)(char* buffer, unsigned len);

void ihex_dump_flash(ihex_flush_cb_t cb);
