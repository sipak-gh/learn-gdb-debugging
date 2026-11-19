/* test_mem.c - tests for mem.c. */
#include "mem.h"
#include "test.h"
#include "tests.h"

void test_mem_format_line(void)
{
    const unsigned char bytes[] = "hello, tdb\0\0\xfe\xff\0";
    char out[160];

    CHECK_STR(mem_format_line(out, sizeof out, 0x555555558010, bytes, 16, "greeting"),
              "0x0000555555558010  68 65 6c 6c 6f 2c 20 74  64 62 00 00 fe ff 00 00  "
              "|hello, tdb......|  <greeting>");
    CHECK_STR(mem_format_line(out, sizeof out, 0x10, bytes, 16, NULL),
              "0x0000000000000010  68 65 6c 6c 6f 2c 20 74  64 62 00 00 fe ff 00 00  "
              "|hello, tdb......|");
}

void test_mem_format_short_line(void)
{
    const unsigned char bytes[] = { 0xf3, 0x0f, 0x1e, 0xfa, 0x55 };
    char out[160];

    /* The hex columns keep their width, so the text column lines up. */
    CHECK_STR(mem_format_line(out, sizeof out, 0x1149, bytes, 5, "square"),
              "0x0000000000001149  f3 0f 1e fa 55                                    "
              "|....U|  <square>");
    CHECK_STR(mem_format_line(out, 20, 0x1149, bytes, 5, NULL), "0x0000000000001149 ");
}
