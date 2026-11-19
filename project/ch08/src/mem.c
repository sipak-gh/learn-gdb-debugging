/* mem.c - showing memory as a hex dump. */
#include "mem.h"

#include <stdio.h>

char *mem_format_line(char *out, size_t size, uint64_t addr, const unsigned char *bytes,
                      size_t n, const char *label)
{
    char hex[3 * MEM_LINE + 2] = "";
    char text[MEM_LINE + 1] = "";
    size_t h = 0;

    if (n > MEM_LINE)
        n = MEM_LINE;
    for (size_t i = 0; i < MEM_LINE; i++) {
        if (i == MEM_LINE / 2)
            hex[h++] = ' ';     /* a gap after 8 bytes */
        if (i < n) {
            snprintf(hex + h, sizeof hex - h, "%02x ", bytes[i]);
            text[i] = bytes[i] >= 0x20 && bytes[i] < 0x7f ? (char)bytes[i] : '.';
        } else {
            snprintf(hex + h, sizeof hex - h, "   ");
        }
        h += 3;
    }
    text[n] = '\0';
    snprintf(out, size, "0x%016llx  %s |%s|%s%s%s", (unsigned long long)addr, hex, text,
             label ? "  <" : "", label ? label : "", label ? ">" : "");
    return out;
}
