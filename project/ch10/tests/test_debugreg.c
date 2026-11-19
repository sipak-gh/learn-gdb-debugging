/* test_debugreg.c - tests for the DR7 encoding (debugreg.c) and the
 * system call names (syscalls.c). */
#include "debugreg.h"
#include "syscalls.h"
#include "test.h"
#include "tests.h"

void test_dr7_enable(void)
{
    /* Slot 0, write, 4 bytes: L0 (bit 0), R/W0 = 01 (bits 16-17),
     * LEN0 = 11 (bits 18-19). */
    CHECK_INT(dr7_enable(0, 0, DR_WRITE, 4), 0xd0001);
    /* Slot 1, read/write, 8 bytes: L1 (bit 2), R/W1 = 11 (bits 20-21),
     * LEN1 = 10 (bits 22-23). */
    CHECK_INT(dr7_enable(0, 1, DR_ACCESS, 8), 0xb00004);
    /* Slot 3, write, 1 byte: L3 (bit 6), R/W3 = 01 (bit 28), LEN3 = 00. */
    CHECK_INT(dr7_enable(0, 3, DR_WRITE, 1), 0x10000040);
    /* Slot 2, write, 2 bytes: LEN2 = 01 (bit 26). */
    CHECK_INT(dr7_enable(0, 2, DR_WRITE, 2), 0x5000010);
}

void test_dr7_disable(void)
{
    uint64_t dr7 = dr7_enable(dr7_enable(0, 0, DR_WRITE, 4), 1, DR_ACCESS, 8);

    CHECK_INT(dr7_disable(dr7, 0), 0xb00004);         /* slot 1 is untouched */
    CHECK_INT(dr7_disable(dr7_disable(dr7, 0), 1), 0);
    /* Enabling a slot again replaces its old fields. */
    CHECK_INT(dr7_enable(dr7, 0, DR_ACCESS, 1), 0xb30005);
}

void test_sys_names(void)
{
    CHECK_STR(sys_name(0), "read");
    CHECK_STR(sys_name(1), "write");
    CHECK_STR(sys_name(257), "openat");
    CHECK_STR(sys_name(59), "execve");
    CHECK(sys_name(-1) == NULL);
    CHECK(sys_name(100000) == NULL);
    CHECK_INT(sys_number("write"), 1);
    CHECK_INT(sys_number("openat"), 257);
    CHECK_INT(sys_number("60"), 60);
    CHECK_INT(sys_number("nosuch"), -1);
    CHECK_INT(sys_number("12x"), -1);
    CHECK_INT(sys_number("100000"), -1);
}
