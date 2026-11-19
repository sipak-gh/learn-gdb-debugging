/* mem.h - showing memory as a hex dump. */
#ifndef TDB_MEM_H
#define TDB_MEM_H

#include <stddef.h>
#include <stdint.h>

/* Bytes per line of a dump. */
#define MEM_LINE 16

/* Format one line of a dump, like hexdump -C:
 *
 *   0x0000555555558010  68 65 6c 6c 6f 2c 20 74  64 62 00 00 00 00 00 00  |hello, tdb......|
 *
 * addr is the address of bytes[0]; n (at most MEM_LINE) bytes are shown.
 * If label is not NULL, "  <label>" is added at the end. Returns out. */
char *mem_format_line(char *out, size_t size, uint64_t addr, const unsigned char *bytes,
                      size_t n, const char *label);

#endif
